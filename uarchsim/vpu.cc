#include "vpu.h"
#include <cstring>

//============================================================
// Constructor / Destructor
//============================================================
vpu_t::vpu_t(uint32_t vpq_size, uint32_t num_chkpts,
             uint32_t index_bits, uint32_t tag_bits,
             uint32_t conf_max, uint32_t conf_miss_pen,
             bool oracle_conf) {

    this->vpq_size      = vpq_size;
    this->num_chkpts    = num_chkpts;
    this->index_bits    = index_bits;
    this->tag_bits      = tag_bits;
    this->pt_index_bits = index_bits + 1;       // PT is 2x VHT
    this->conf_max      = conf_max;
    this->conf_miss_pen = conf_miss_pen;
    this->oracle_conf   = oracle_conf;

    vht_num_entries = (1u << index_bits);
    pt_num_entries  = (1u << pt_index_bits);

    //-----------------------------
    // VHT init
    //-----------------------------
    vht = new vht_entry_t[vht_num_entries];
    for (uint32_t i = 0; i < vht_num_entries; i++) {
        vht[i].valid      = true;   //always true. tag and index are there to verify the validity. This is correct as per spec
        vht[i].tag        = 0;
        vht[i].last_value = 0;
    }

    //-----------------------------
    // PT init
    //-----------------------------
    pt = new pt_entry_t[pt_num_entries];
    for (uint32_t i = 0; i < pt_num_entries; i++) {
        pt[i].value = 0;
        pt[i].conf  = 0;
    }

    //-----------------------------
    // VPQ init
    //-----------------------------
    vpq_head       = 0;
    vpq_tail       = 0;
    vpq_head_phase = false;
    vpq_tail_phase = false;

    vpq = new vpq_entry_t[vpq_size];
    for (uint32_t i = 0; i < vpq_size; i++) {
        vpq[i].pc          = 0;
        vpq[i].valid       = false;
        vpq[i].value       = 0;
        vpq[i].value_ready = false;
        vpq[i].vp_val      = 0;
        vpq[i].predicted   = false;
        vpq[i].confident   = false;
        vpq[i].fwd_val     = 0;
        vpq[i].fwd_valid   = false;
        vpq[i].flags       = 0;
    }

    //-----------------------------
    // Checkpoint arrays
    //-----------------------------
    vpq_checkpoint_tail       = new uint32_t[num_chkpts];
    vpq_checkpoint_tail_phase = new bool[num_chkpts];
    memset(vpq_checkpoint_tail,       0, sizeof(uint32_t) * num_chkpts);
    memset(vpq_checkpoint_tail_phase, 0, sizeof(bool)     * num_chkpts);
}

vpu_t::~vpu_t() {
    delete[] vht;
    delete[] pt;
    delete[] vpq;
    delete[] vpq_checkpoint_tail;
    delete[] vpq_checkpoint_tail_phase;
}

//============================================================
// VHT / PT index and tag
//============================================================
// VHT index: PC bits [index_bits+1 : 2]
uint32_t vpu_t::vht_get_index(uint64_t pc) {
    uint64_t mask = (1ULL << index_bits) - 1;
    return (uint32_t)((pc >> 2) & mask);
}

// VHT tag: PC bits [index_bits+tag_bits+1 : index_bits+2]
uint64_t vpu_t::vht_get_tag(uint64_t pc) {
    if (tag_bits == 0) return 0;
    uint64_t mask = (1ULL << tag_bits) - 1;
    return (pc >> (2 + index_bits)) & mask;
}

// PT index: simple XOR-fold of last_value, XORed with VHT index
uint32_t vpu_t::pt_get_index(uint32_t vht_idx, uint64_t last_value) {
    uint64_t mask = (1ULL << pt_index_bits) - 1;

    // Fold 64-bit last_value down to pt_index_bits
    // Step 1: XOR upper 32 bits with lower 32 bits
    uint64_t folded = last_value ^ (last_value >> 32);
    // Step 2: XOR upper pt_index_bits with lower pt_index_bits
    folded ^= (folded >> pt_index_bits);
    folded &= mask;

    // Mix in VHT index
    return (uint32_t)(folded ^ (uint64_t)vht_idx);
}

//============================================================
// VPQ operations
//============================================================
uint32_t vpu_t::vpq_alloc(uint64_t pc) {
    assert(!full());
    uint32_t idx         = vpq_tail;
    vpq[idx].pc          = pc;
    vpq[idx].valid       = true;
    vpq[idx].value       = 0;
    vpq[idx].value_ready = false;
    vpq[idx].vp_val      = 0;
    vpq[idx].predicted   = false;
    vpq[idx].confident   = false;
    vpq[idx].fwd_val     = 0;
    vpq[idx].fwd_valid   = false;
    vpq[idx].flags       = 0;

    vpq_tail++;
    if (vpq_tail == vpq_size) {
        vpq_tail       = 0;
        vpq_tail_phase = !vpq_tail_phase;
    }
    return idx;
}

void vpu_t::vpq_write_value(uint32_t vpq_idx, uint64_t value) {
    assert(vpq[vpq_idx].valid);
    vpq[vpq_idx].value       = value;
    vpq[vpq_idx].value_ready = true;
}

void vpu_t::vpq_checkpoint(uint32_t branch_ID) {
    assert(branch_ID < num_chkpts);
    vpq_checkpoint_tail[branch_ID]       = vpq_tail;
    vpq_checkpoint_tail_phase[branch_ID] = vpq_tail_phase;
}

void vpu_t::vpq_repair(uint32_t branch_ID) {
    assert(branch_ID < num_chkpts);
    repair_instances(vpq_checkpoint_tail[branch_ID],
                     vpq_checkpoint_tail_phase[branch_ID]);
}

//============================================================
// Backward walk helper — unchanged from infinite version
//============================================================
bool vpu_t::backward_walk_find_context(uint64_t pc, uint32_t new_vpq_idx,
                                        uint64_t &ctx) {
    uint32_t entries_in_flight;
    if (vpq_tail_phase == vpq_head_phase)
        entries_in_flight = vpq_tail - vpq_head;
    else
        entries_in_flight = vpq_size - vpq_head + vpq_tail;

    for (uint32_t n = 0; n < entries_in_flight; n++) {
        uint32_t pos = (vpq_tail + vpq_size - 1 - n) % vpq_size;

        if (!vpq[pos].valid)     continue;
        if (pos == new_vpq_idx)  continue;
        if (vpq[pos].pc != pc)   continue;

        if (vpq[pos].value_ready) {
            ctx = vpq[pos].value;
            return true;
        }
        if (vpq[pos].fwd_valid) {
            ctx = vpq[pos].fwd_val;
            return true;
        }
        return false;
    }

    // Fall back to VHT with tag check
    uint32_t vht_idx = vht_get_index(pc);
    uint64_t vht_tag = vht_get_tag(pc);
    if (vht[vht_idx].valid &&
        (tag_bits == 0 || vht[vht_idx].tag == vht_tag)) {
        ctx = vht[vht_idx].last_value;
        return true;
    }

    return false;
}

//============================================================
// predict()
//============================================================
void vpu_t::predict(uint64_t pc, uint32_t vpq_idx, uint64_t actual_value) {

    // Step 1: find context via backward walk
    uint64_t ctx;
    bool have_ctx = backward_walk_find_context(pc, vpq_idx, ctx);

    if (!have_ctx) {
        vpq[vpq_idx].predicted = false;
        vpq[vpq_idx].confident = false;
        vpq[vpq_idx].vp_val    = 0;
        return;
    }

    // Step 2: look up PT with hash(vht_idx, ctx)
    uint32_t vht_idx  = vht_get_index(pc);
    uint32_t pt_idx   = pt_get_index(vht_idx, ctx);
    pt_entry_t &entry = pt[pt_idx];

    // PT has no tag — always a "hit" but may be aliased
    // Confidence counter filters out bad aliases

    bool confident;
    if (oracle_conf)
        confident = (entry.value == actual_value);
    else
        confident = (entry.conf >= conf_max);

    vpq[vpq_idx].predicted = true;
    vpq[vpq_idx].vp_val    = entry.value;
    vpq[vpq_idx].confident = confident;
}

//============================================================
// forward_walk() — uses conf_max/2 threshold
//============================================================
void vpu_t::forward_walk(uint32_t vpq_idx) {
    if (!vpq[vpq_idx].valid) return;
    if (!vpq[vpq_idx].value_ready) return;

    uint64_t pc          = vpq[vpq_idx].pc;
    uint64_t current_ctx = vpq[vpq_idx].value;

    uint32_t vht_idx     = vht_get_index(pc);

    // Forward walk threshold — half of conf_max
    uint32_t fwd_threshold = conf_max / 2;
    if (fwd_threshold == 0) fwd_threshold = 1;

    uint32_t steps = 0;
    uint32_t p     = (vpq_idx + 1) % vpq_size;

    while (steps < vpq_size) {
        if (p == vpq_tail) break;

        if (!vpq[p].valid) {
            p = (p + 1) % vpq_size;
            steps++;
            continue;
        }

        if (vpq[p].pc == pc) {
            // Stop if this instance already has its real value
            if (vpq[p].value_ready) break;

            // Look up PT
            uint32_t pt_idx   = pt_get_index(vht_idx, current_ctx);
            pt_entry_t &entry = pt[pt_idx];

            if (entry.conf < fwd_threshold) {
                break;   // chain broken
            }

            // Deposit prediction
            vpq[p].fwd_val   = entry.value;
            vpq[p].fwd_valid = true;

            // Update context for next iteration
            current_ctx = entry.value;
        }

        p = (p + 1) % vpq_size;
        steps++;
    }
}

//============================================================
// train()
//============================================================
void vpu_t::train(uint32_t vpq_idx) {
    assert(vpq_idx == vpq_head);
    assert(vpq[vpq_idx].valid);
    assert(vpq[vpq_idx].value_ready);

    uint64_t pc    = vpq[vpq_idx].pc;
    uint64_t value = vpq[vpq_idx].value;

    uint32_t vht_idx = vht_get_index(pc);
    uint64_t vht_tag = vht_get_tag(pc);

    // Step 1: if VHT hit (valid + tag match), update PT
    bool vht_hit = vht[vht_idx].valid &&
                   (tag_bits == 0 || vht[vht_idx].tag == vht_tag);

    if (vht_hit) {
        uint64_t last_value = vht[vht_idx].last_value;
        uint32_t pt_idx     = pt_get_index(vht_idx, last_value);
        pt_entry_t &entry   = pt[pt_idx];

        if (entry.value == value) {
            // Correct — increment confidence
            if (entry.conf < conf_max) entry.conf++;
        } else {
            // Wrong — apply miss penalty, update value
            if (entry.conf >= conf_miss_pen)
                entry.conf -= conf_miss_pen;
            else
                entry.conf = 0;
            entry.value = value;
        }
    }

    // Step 2: update VHT (always — allocate or overwrite)
    vht[vht_idx].valid      = true;
    vht[vht_idx].tag        = vht_tag;
    vht[vht_idx].last_value = value;

    // Step 3: free VPQ head
    vpq[vpq_idx].valid       = false;
    vpq[vpq_idx].value_ready = false;
    vpq_head++;
    if (vpq_head == vpq_size) {
        vpq_head       = 0;
        vpq_head_phase = !vpq_head_phase;
    }
}

//============================================================
// repair_instances() — unchanged logic
//============================================================
void vpu_t::repair_instances(uint32_t rollback_tail, bool rollback_tail_phase) {
    uint32_t entries_to_free;
    if (vpq_tail_phase == rollback_tail_phase)
        entries_to_free = vpq_tail - rollback_tail;
    else
        entries_to_free = vpq_size - rollback_tail + vpq_tail;

    for (uint32_t n = 0; n < entries_to_free; n++) {
        if (vpq_tail == 0) {
            vpq_tail       = vpq_size - 1;
            vpq_tail_phase = !vpq_tail_phase;
        } else {
            vpq_tail--;
        }

        if (vpq[vpq_tail].valid) {
            vpq[vpq_tail].valid       = false;
            vpq[vpq_tail].value_ready = false;
            vpq[vpq_tail].fwd_valid   = false;
        }
    }

    assert(vpq_tail       == rollback_tail);
    assert(vpq_tail_phase == rollback_tail_phase);
}

//============================================================
// full_flush() — unchanged logic
//============================================================
void vpu_t::full_flush() {
    uint32_t entries_to_free;
    if (vpq_tail_phase == vpq_head_phase)
        entries_to_free = vpq_tail - vpq_head;
    else
        entries_to_free = vpq_size - vpq_head + vpq_tail;

    uint32_t i = vpq_head;
    for (uint32_t n = 0; n < entries_to_free; n++) {
        if (vpq[i].valid) {
            vpq[i].valid       = false;
            vpq[i].value_ready = false;
            vpq[i].fwd_valid   = false;
        }
        i = (i + 1) % vpq_size;
    }

    vpq_tail       = vpq_head;
    vpq_tail_phase = vpq_head_phase;
}