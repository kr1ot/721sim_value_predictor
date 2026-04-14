#include "vpu.h"
#include <cstring>

vpu_t::vpu_t(uint32_t vpq_size,
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

    svp_num_entries = (1u << index_bits);

    //vpq circular buffer
    vpq_head        = 0;
    vpq_tail        = 0;
    vpq_count       = 0;

    svp = new svp_entry_t[svp_num_entries];
    //invalidate all the entries of the SVP table
    //they will updated when adding an entry in the table
    for (uint64_t i = 0; i < svp_num_entries; i++){
        svp[i].valid = false;
    }

    vpq = new vpq_entry_t[vpq_size];
    //same for VPQ
    for (uint64_t i = 0; i < vpq_size; i++){
        vpq[i].valid = false;
    }
}

vpu_t::~vpu_t() {
    delete[] svp;
    delete[] vpq;
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

//check if VPQ has a free entry
bool vpu_t::vpq_has_free() {
    return (vpq_count < vpq_size);
}

//count the number of free entries in VPQ
uint32_t vpu_t::vpq_free_count() {
    return vpq_size - vpq_count;
}

/////////////////////////////////
// Prediction from VPU
/////////////////////////////////
bool vpu_t::predict(uint64_t  pc,
                    uint64_t &pred_value,
                    bool     &confident,
                    uint32_t &vpq_tail_out,
                    uint64_t  actual_value)
{
    uint64_t pc_index = get_index(pc);
    uint64_t pc_tag = get_tag(pc);

    //index into SVP table using pc index
    svp_entry_t &svp_entry = svp[pc_index];

    // hit = entry valid AND tag matches (or no tags used)
    bool hit = svp_entry.valid && (svp_entry.tag == pc_tag);

    //if SVP entry missed,
    if (!hit) {
        confident    = false;
        pred_value   = 0;
        vpq_tail_out = vpq_tail;
        return false;
    }
    //if SVP entry hit

    // speculatively increment instance
    svp_entry.instance++;

    // prediction = retired_value + instance * stride
    pred_value = (uint64_t)((int64_t)svp_entry.retired_value + svp_entry.instance * svp_entry.stride);

    // decide the confidence
    if (oracle_conf) {
        // oracle mode: confident only if prediction matches actual checker value
        confident = (pred_value == actual_value);
    } 
    else {
        // real mode: confident if conf == conf_max
        confident = (svp_entry.conf == conf_max);
    }

    //provide the tail index at which this prediction was done
    vpq_tail_out = vpq_tail;
    //return true since prediction was made
    return true;
}


uint32_t vpu_t::vpq_alloc(uint64_t pc) {
    //Should have reached here only if there were some free entries   
    assert(vpq_count < vpq_size);

    //get the tail index where new entry is to be allocated in VPQ
    uint32_t idx     = vpq_tail;
    vpq[idx].pc          = pc;
    vpq[idx].value       = 0;
    vpq[idx].value_ready = false;
    vpq[idx].valid       = true;

    vpq_tail++;
    if (vpq_tail == vpq_size) {
        vpq_tail = 0;
    }
    //increment the vpq count to track the number of entries in the VPQ
    vpq_count++;
    //return the index where entry was allocated
    return idx;
}

//write value to the VPQ index
void vpu_t::vpq_write_value(uint32_t vpq_idx, uint64_t value) {
    //only write if the entry was valid
    assert(vpq[vpq_idx].valid);
    vpq[vpq_idx].value       = value;
    vpq[vpq_idx].value_ready = true;
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
    assert(vpq[vpq_idx].value_ready);

    //get the necessary variables
    uint64_t pc = vpq[vpq_idx].pc;
    uint64_t value = vpq[vpq_idx].value;
    uint64_t pc_index = get_index(pc);
    uint64_t pc_tag = get_tag(pc);
    svp_entry_t &svp_entry = svp[pc_index];

    //check if the entry hit in SVP
    bool hit = svp_entry.valid && (svp_entry.tag == pc_tag);

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
        
        // Walk VPQ from H+1 to T to count matching in-flight entries
        // This sets the initial instance counter for the new entry
        uint32_t in_flight = 0;
        //since not including head
        uint32_t remaining = vpq_count - 1;
        //go through all the entries till tail
        for (uint32_t i = 0; i < remaining; i++) {
            //ensure roll over
            uint32_t pos = (vpq_head + 1 + i) % vpq_size;
            if (vpq[pos].valid) {
                //if the pc exists in the VPQ
                if (pc == vpq[pos].pc) {
                in_flight++;
                }
            }
        }

        //initialize the SVp entry
        svp_entry.valid         = true;
        svp_entry.tag           = pc_tag;
        svp_entry.conf          = 0;
        svp_entry.retired_value = value;
        svp_entry.stride        = (int64_t)value;  // retired_value = stride = value 
        svp_entry.instance      = in_flight;
    }

    // Free VPQ head
    vpq[vpq_idx].valid = false;
    vpq_head  = (vpq_head + 1) % vpq_size;
    vpq_count--;
}

//repair after a squash
void vpu_t::repair_instances(uint32_t rollback_tail) {

    while (vpq_tail != rollback_tail) {
        //roll over logic for vpq_tail
        vpq_tail = (vpq_tail == 0) ? (vpq_size - 1) : (vpq_tail - 1);

        if (vpq[vpq_tail].valid) {
            uint64_t pc_index = get_index(vpq[vpq_tail].pc);
            uint64_t pc_tag = get_tag(vpq[vpq_tail].pc);
            svp_entry_t &svp_entry = svp[pc_index];

            bool hit = svp_entry.valid && ((tag_bits == 0) || (svp_entry.tag == pc_tag));
            if (hit) svp_entry.instance--;   // undo speculative increment from Rename

            vpq[vpq_tail].valid = false;
            vpq_count--;
        }
    }
}

void vpu_t::full_flush() {
    // Walk forward from head to tail, repairing instance counters
    // and freeing ALL entries including the head
    uint32_t i = vpq_head;
    uint32_t remaining = vpq_count;

    for (uint32_t n = 0; n < remaining; n++) {
        if (vpq[i].valid) {
            uint64_t pc_index = get_index(vpq[i].pc);
            uint64_t pc_tag   = get_tag(vpq[i].pc);
            svp_entry_t &entry = svp[pc_index];

            bool hit = entry.valid &&
                        ((tag_bits == 0) || (entry.tag == pc_tag));
            if (hit) entry.instance--;

            vpq[i].valid       = false;
            vpq[i].value_ready = false;
        }
        i = (i + 1) % vpq_size;
    }

    // Reset VPQ to empty
    vpq_tail  = vpq_head;
    vpq_count = 0;
}

//Storage cose for VPU
uint64_t vpu_t::svp_storage_bytes() {
   uint32_t conf_bits = 0;
   uint64_t cm = conf_max;
   while (cm > 0) { conf_bits++; cm >>= 1; }
   if (conf_bits == 0) conf_bits = 1;

   uint32_t bits_per_entry = tag_bits + conf_bits + 64 + 64 + 64;
   uint64_t total_bits     = (uint64_t)bits_per_entry * svp_num_entries;
   return (total_bits + 7) / 8;
}
