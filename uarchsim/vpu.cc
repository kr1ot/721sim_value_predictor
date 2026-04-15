#include "vpu.h"
#include <cassert>

vpu_t::vpu_t(uint32_t vpq_size,
             uint32_t num_chkpts,
             uint32_t index_bits,
             uint32_t tag_bits,
             uint64_t conf_max,
             bool     oracle_conf)
{
    this->vpq_size        = vpq_size;
    this->index_bits      = index_bits;
    this->tag_bits        = tag_bits;
    this->conf_max        = conf_max;
    this->oracle_conf     = oracle_conf;
    this->num_chkpts      = num_chkpts;

    svp_num_entries = (1u << index_bits);

    //vpq circular buffer
    vpq_head        = 0;
    vpq_tail        = 0;
    vpq_head_phase = false;
    vpq_tail_phase = false;

    svp = new svp_entry_t[svp_num_entries];
    //invalidate all the entries of the SVP table
    //they will updated when adding an entry in the table
    for (uint64_t i = 0; i < svp_num_entries; i++){
        svp[i].tag = 0;
        svp[i].conf = 0;
        svp[i].retired_value= 0;
        svp[i].stride = 0;
        svp[i].instance = 0;
    }

    vpq = new vpq_entry_t[vpq_size];
    //same for VPQ
    for (uint64_t i = 0; i < vpq_size; i++){
        vpq[i].valid = false; //TODO: check if required

        vpq[i].predicted = false;
        vpq[i].confident = false;
        vpq[i].pc = 0;
        vpq[i].value = 0;
        vpq[i].vp_val = 0;
    }

    vpq_checkpoint_tail = new uint32_t[num_chkpts];
    vpq_checkpoint_tail_phase = new bool[num_chkpts];
    for (uint32_t i = 0; i < num_chkpts;i++){
        vpq_checkpoint_tail[i] = 0;
        vpq_checkpoint_tail_phase[i] = false;
    }

}

vpu_t::~vpu_t() {
    delete[] svp;
    delete[] vpq;
    delete[] vpq_checkpoint_tail;
    delete[] vpq_checkpoint_tail_phase;
}

//index and tag related functions
// PC layout :
//   bits [1:0]               = 00, discard (RISC-V aligned)
//   bits [index_bits+1 : 2]  = PCindex
//   bits [63 : index_bits+2] = PCtag 

//get the index from PC
uint64_t vpu_t::get_index(uint64_t pc) {
    uint64_t mask = (1ULL << index_bits) - 1;
    return (pc >> 2) & mask;
}

//get the tag from the PC
uint64_t vpu_t::get_tag(uint64_t pc) {
    if (tag_bits == 0) return 0;
    uint64_t mask = (1ULL << tag_bits) - 1;
    return (pc >> (2 + index_bits)) & mask;
}

uint32_t vpu_t::vpq_free_count() {
    if (vpq_tail_phase == vpq_head_phase)
        return vpq_size - (vpq_tail - vpq_head);
    else
        return vpq_head - vpq_tail;
}

uint32_t vpu_t::vpq_alloc(uint64_t pc) {

    //get the tail index where new entry is to be allocated in VPQ
    uint32_t idx = vpq_tail;

    vpq[idx].pc = pc;
    vpq[idx].value = 0;
    //TODO: Check if required
    vpq[idx].valid = true;

    //speculative
    vpq[idx].vp_val = 0;
    vpq[idx].predicted = false;
    vpq[idx].confident = false;

    vpq_tail++;
    if (vpq_tail == vpq_size) {
        vpq_tail = 0;
        vpq_tail_phase = !vpq_tail_phase;
    }
    //return the index where entry was allocated
    return idx;
}

//write value to the VPQ index
void vpu_t::vpq_write_value(uint32_t vpq_idx, uint64_t value) {
    //only write if the entry was valid
    assert(vpq[vpq_idx].valid);   //only used as debug

    vpq[vpq_idx].value       = value;
}

void vpu_t::vpq_checkpoint(uint32_t branch_ID) {
    vpq_checkpoint_tail[branch_ID] = vpq_tail;
    vpq_checkpoint_tail_phase[branch_ID] = vpq_tail_phase;
}

void vpu_t::vpq_repair(uint32_t branch_ID) {
    repair_instances(vpq_checkpoint_tail[branch_ID],
                     vpq_checkpoint_tail_phase[branch_ID]);
}

/////////////////////////////////
// Prediction from VPU
/////////////////////////////////
void vpu_t::predict(uint64_t  pc,
                    uint32_t vpq_idx,
                    uint64_t  actual_value)
{
    uint64_t pc_index = get_index(pc);
    uint64_t pc_tag = get_tag(pc);

    //index into SVP table using pc index
    svp_entry_t &svp_entry = svp[pc_index];

    // hit = entry valid AND tag matches (or no tags used)
    bool hit = svp_entry.tag == pc_tag;

    //if SVP entry missed,
    if (!hit) {
        vpq[vpq_idx].confident = false;
        vpq[vpq_idx].predicted = false;
        vpq[vpq_idx].vp_val = 0;
        return;
    }
    //if SVP entry hit

    // speculatively increment instance
    svp_entry.instance++;

    // prediction = retired_value + instance * stride
    uint64_t pred_value = (uint64_t)((int64_t)svp_entry.retired_value + svp_entry.instance * svp_entry.stride);

    bool confident;
    // decide the confidence
    if (oracle_conf) {
        // oracle mode: confident only if prediction matches actual checker value
        confident = (pred_value == actual_value);
    } 
    else {
        // real mode: confident if conf == conf_max
        confident = (svp_entry.conf == conf_max);
    }

    vpq[vpq_idx].confident = confident;
    vpq[vpq_idx].predicted = true;
    vpq[vpq_idx].vp_val = pred_value;
}

///////////////////////////////////
// Training of VPU
///////////////////////////////////

void vpu_t::train(uint32_t vpq_idx) {
    //check if the index is same as the head of the vpq
    assert(vpq_idx == vpq_head);
    //whether the entry is valid and has the value ready by the moment
    //this function was called
    assert(vpq[vpq_idx].valid);

    //get the necessary variables
    uint64_t pc = vpq[vpq_idx].pc;
    uint64_t value = vpq[vpq_idx].value;
    uint64_t pc_index = get_index(pc);
    uint64_t pc_tag = get_tag(pc);
    svp_entry_t &svp_entry = svp[pc_index];

    //check if the entry hit in SVP
    bool hit = (svp_entry.tag == pc_tag);

    //if hit then train the svp
    if (hit) {
        // ======== Train  ==========
        //get the new stride
        int64_t new_stride = (int64_t)value - (int64_t)svp_entry.retired_value;
        //if the stride matches,
        if (new_stride == svp_entry.stride) {
            //increase the confidence
            if (svp_entry.conf < conf_max) svp_entry.conf++;   // saturate at conf_max
        } 
        //if stride does not match
        else {
            svp_entry.stride = new_stride;
            svp_entry.conf = 0;
        }
        svp_entry.retired_value = value;
        svp_entry.instance--;   // one in-flight instance retired

    }

    //if miss in SVP, then allocate a new entry/replace
    else {
        // Count in-flight instances by walking VPQ head+1 to tail
        int64_t in_flight = 0;

        uint32_t i = (vpq_head + 1) % vpq_size;
        
        // Use free count to determine how many to walk
        uint32_t to_walk = (vpq_free_count() == 0) ? (vpq_size - 1) :
                           (vpq_size - vpq_free_count() - 1);

        for (uint32_t n = 0; n < to_walk; n++) {
            if (vpq[i].pc == pc)
                in_flight++;
            i = (i + 1) % vpq_size;
        }
        svp_entry.tag           = pc_tag;
        svp_entry.conf          = 0;
        svp_entry.retired_value = value;
        svp_entry.stride        = (int64_t)value;
        svp_entry.instance      = in_flight;
    }

    // Free VPQ head
    vpq[vpq_idx].valid = false;
    vpq_head++;
    if(vpq_head == vpq_size){
        vpq_head = 0;
        vpq_head_phase = !vpq_head_phase;
    }
}

//repair after a squash
void vpu_t::repair_instances(uint32_t rollback_tail, bool rollback_tail_phase) {
    uint32_t entries_to_free;
    //get the entries to free value based on the tail phase
    //tail has not rolled over
    if (vpq_tail_phase == rollback_tail_phase)
        entries_to_free = vpq_tail - rollback_tail;
    //tail has rolled over
    else
        entries_to_free = vpq_size - rollback_tail + vpq_tail;

    for (uint32_t n = 0; n < entries_to_free; n++) {
        if (vpq_tail == 0) {
            vpq_tail       = vpq_size - 1;
            vpq_tail_phase = !vpq_tail_phase;
        } 
        else {
            vpq_tail--;
        }

        uint64_t pc_index = get_index(vpq[vpq_tail].pc);
        uint64_t pc_tag   = get_tag(vpq[vpq_tail].pc);
        svp_entry_t &svp_entry = svp[pc_index];
        bool hit = svp_entry.tag == pc_tag;

        if (hit) svp_entry.instance--;
        vpq[vpq_tail].valid       = false;
    }
    //to be sure I reached the right point
    assert(vpq_tail       == rollback_tail);
    assert(vpq_tail_phase == rollback_tail_phase);
}

void vpu_t::full_flush() {
    uint32_t entries_to_free;

    if (vpq_tail_phase == vpq_head_phase)
        entries_to_free = vpq_tail - vpq_head;
    else
        entries_to_free = vpq_size - vpq_head + vpq_tail;

    uint32_t i = vpq_head;
    for (uint32_t n = 0; n < entries_to_free; n++) {
        uint64_t pc_index = get_index(vpq[i].pc);
        uint64_t pc_tag   = get_tag(vpq[i].pc);

        svp_entry_t &svp_entry = svp[pc_index];
        bool hit = svp_entry.tag == pc_tag;

        if (hit) svp_entry.instance--;

        vpq[i].valid = false;
        i = (i + 1) % vpq_size;
    }
    vpq_tail       = vpq_head;
    vpq_tail_phase = vpq_head_phase;
}