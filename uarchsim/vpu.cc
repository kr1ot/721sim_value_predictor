#include "vpu.h"
#include <cstring>

//============================================================
// Constructor / Destructor
//============================================================
vpu_t::vpu_t(uint32_t vpq_size, uint32_t num_chkpts,
             uint32_t index_bits, uint32_t tag_bits,
             uint32_t conf_max, uint32_t conf_miss_pen,
             bool oracle_conf,
      uint32_t bhr_length) {

    this->vpq_size      = vpq_size;
    this->num_chkpts    = num_chkpts;
    this->index_bits    = index_bits;
    this->tag_bits      = tag_bits;
    this->pt_index_bits = index_bits + 1;
    this->conf_max      = conf_max;
    this->conf_miss_pen = conf_miss_pen;
    this->oracle_conf   = oracle_conf;


    this->bhr_length = bhr_length;
    this->bhr_mask   = (bhr_length == 0) ? 0 :
                    ((1ULL << bhr_length) - 1);
    this->commit_bhr = 0;

    svp_num_entries      = (1u << index_bits);
    vht_num_entries      = (1u << index_bits);
    pt_num_entries       = (1u << pt_index_bits);
    mediator_num_entries = (1u << index_bits);

    //-----------------------------
    // SVP init
    //-----------------------------
    svp = new svp_entry_t[svp_num_entries];
    for (uint32_t i = 0; i < svp_num_entries; i++) {
        svp[i].valid         = true;
        svp[i].tag           = 0;
        svp[i].conf          = 0;
        svp[i].retired_value = 0;
        svp[i].stride        = 0;
        svp[i].instance      = 0;
    }

    //-----------------------------
    // VHT init
    //-----------------------------
    vht = new vht_entry_t[vht_num_entries];
    for (uint32_t i = 0; i < vht_num_entries; i++) {
        vht[i].valid      = true;
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
    // Mediator init — all start at 2 (bias toward SVP)
    //-----------------------------
    mediator = new uint8_t[mediator_num_entries];
    for (uint32_t i = 0; i < mediator_num_entries; i++)
        mediator[i] = 2;

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
        vpq[i].svp_val     = 0;
        vpq[i].svp_hit     = false;
        vpq[i].svp_conf    = false;
        vpq[i].cvp_val     = 0;
        vpq[i].cvp_hit     = false;
        vpq[i].cvp_conf    = false;
        vpq[i].used_svp    = false;
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
    delete[] svp;
    delete[] vht;
    delete[] pt;
    delete[] mediator;
    delete[] vpq;
    delete[] vpq_checkpoint_tail;
    delete[] vpq_checkpoint_tail_phase;
}

void vpu_t::update_commit_bhr(bool taken) {
    if (bhr_length == 0) return;
    commit_bhr = ((commit_bhr << 1) | (taken ? 1ULL : 0ULL)) & bhr_mask;
}

//============================================================
// Index / tag helpers
//============================================================
uint32_t vpu_t::get_index(uint64_t pc) {
    uint64_t mask = (1ULL << index_bits) - 1;
    return (uint32_t)((pc >> 2) & mask);
}

// BHR-mixed index — used by VHT only
uint32_t vpu_t::get_mediator_index(uint64_t pc) {
    uint64_t pc_idx  = (pc >> 2) & ((1ULL << index_bits) - 1);
    uint64_t bhr_idx = commit_bhr & ((1ULL << index_bits) - 1);
    return (uint32_t)(pc_idx ^ bhr_idx);
}


uint64_t vpu_t::get_tag(uint64_t pc) {
    if (tag_bits == 0) return 0;
    uint64_t mask = (1ULL << tag_bits) - 1;
    return (pc >> (2 + index_bits)) & mask;
}

uint32_t vpu_t::pt_get_index(uint32_t vht_idx, uint64_t last_value) {
    uint64_t mask = (1ULL << pt_index_bits) - 1;
    uint64_t folded = last_value ^ (last_value >> 32);
    folded ^= (folded >> pt_index_bits);
    folded &= mask;
    return (uint32_t)(folded ^ (uint64_t)vht_idx);
}

//============================================================
// VPQ operations
//============================================================
uint32_t vpu_t::vpq_alloc(uint64_t pc) {
    assert(!full());
    uint32_t idx = vpq_tail;
    vpq[idx].pc          = pc;
    vpq[idx].valid       = true;
    vpq[idx].value       = 0;
    vpq[idx].value_ready = false;
    vpq[idx].vp_val      = 0;
    vpq[idx].predicted   = false;
    vpq[idx].confident   = false;
    vpq[idx].fwd_val     = 0;
    vpq[idx].fwd_valid   = false;
    vpq[idx].svp_val     = 0;
    vpq[idx].svp_hit     = false;
    vpq[idx].svp_conf    = false;
    vpq[idx].cvp_val     = 0;
    vpq[idx].cvp_hit     = false;
    vpq[idx].cvp_conf    = false;
    vpq[idx].used_svp    = false;

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
// Backward walk for CVP context
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

        if (!vpq[pos].valid)    continue;
        if (pos == new_vpq_idx) continue;
        if (vpq[pos].pc != pc)  continue;

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

    // Fall back to VHT -> get context from retired values 
    uint32_t vht_idx = get_index(pc);
    uint64_t vht_tag = get_tag(pc);
    if (vht[vht_idx].valid &&
        (tag_bits == 0 || vht[vht_idx].tag == vht_tag)) {
        ctx = vht[vht_idx].last_value;
        return true;
    }
    return false;
}

//============================================================
// SVP lookup — speculatively advances instance counter
//============================================================
void vpu_t::svp_lookup(uint64_t pc, uint64_t actual_value,
                        uint64_t &pred_val, bool &hit, bool &confident) {
    uint32_t idx = get_index(pc);
    uint64_t tag = get_tag(pc);
    svp_entry_t &entry = svp[idx];

    hit = (tag_bits == 0 || entry.tag == tag);

    if (!hit) {
        pred_val  = 0;
        confident = false;
        return;
    }

    // Speculatively increment instance
    entry.instance++;
    pred_val = (uint64_t)((int64_t)entry.retired_value +
                           entry.instance * entry.stride);

    if (oracle_conf)
        confident = (pred_val == actual_value);
    else
        confident = (entry.conf >= conf_max);
}

//============================================================
// CVP lookup — uses VHT/backward-walk context + PT
//============================================================
void vpu_t::cvp_lookup(uint64_t pc, uint32_t vpq_idx, uint64_t actual_value,
                        uint64_t &pred_val, bool &hit, bool &confident) {
    uint64_t ctx;
    bool have_ctx = backward_walk_find_context(pc, vpq_idx, ctx);

    if (!have_ctx) {
        hit       = false;
        pred_val  = 0;
        confident = false;
        return;
    }

    uint32_t vht_idx = get_index(pc);
    uint32_t pt_idx  = pt_get_index(vht_idx, ctx);
    pt_entry_t &e    = pt[pt_idx];

    hit      = true;  // PT has no tag — always a hit
    pred_val = e.value;

    if (oracle_conf)
        confident = (e.value == actual_value);
    else
        confident = (e.conf >= conf_max);
}

//============================================================
// predict() — hybrid with mediator
//============================================================
void vpu_t::predict(uint64_t pc, uint32_t vpq_idx, uint64_t actual_value) {
    // Look up both predictors
    uint64_t svp_val = 0, cvp_val = 0;
    bool svp_hit = false, svp_conf = false;
    bool cvp_hit = false, cvp_conf = false;

    svp_lookup(pc, actual_value, svp_val, svp_hit, svp_conf);
    cvp_lookup(pc, vpq_idx, actual_value, cvp_val, cvp_hit, cvp_conf);

    // Save both predictions in VPQ for mediator update at retirement
    vpq[vpq_idx].svp_val  = svp_val;
    vpq[vpq_idx].svp_hit  = svp_hit;
    vpq[vpq_idx].svp_conf = svp_conf;
    vpq[vpq_idx].cvp_val  = cvp_val;
    vpq[vpq_idx].cvp_hit  = cvp_hit;
    vpq[vpq_idx].cvp_conf = cvp_conf;

    // Consult mediator
    // uint32_t med_idx = get_index(pc);
    uint32_t med_idx = get_mediator_index(pc);
    uint8_t  med_val = mediator[med_idx];
    bool     prefer_svp = (med_val >= 2);

    // Selection logic
    bool     use_svp   = false;
    bool     injected  = false;
    uint64_t inj_val   = 0;

    if (prefer_svp) {
        if (svp_conf) {
            use_svp = true; injected = true; inj_val = svp_val;
        } else if (cvp_conf) {
            use_svp = false; injected = true; inj_val = cvp_val;
        }
    } else {
        if (cvp_conf) {
            use_svp = false; injected = true; inj_val = cvp_val;
        } else if (svp_conf) {
            use_svp = true; injected = true; inj_val = svp_val;
        }
    }

    if (injected) {
        vpq[vpq_idx].predicted = true;
        vpq[vpq_idx].confident = true;
        vpq[vpq_idx].vp_val    = inj_val;
        vpq[vpq_idx].used_svp  = use_svp;
    } else {
        // Neither confident — report best effort
        vpq[vpq_idx].predicted = svp_hit || cvp_hit;
        vpq[vpq_idx].confident = false;
        vpq[vpq_idx].vp_val    = prefer_svp ? svp_val : cvp_val;
        vpq[vpq_idx].used_svp  = prefer_svp;
    }
}

//============================================================
// forward_walk() — CVP context propagation only
// SVP does not need this (uses its own instance counter)
//============================================================
void vpu_t::forward_walk(uint32_t vpq_idx) {
    if (!vpq[vpq_idx].valid) return;
    if (!vpq[vpq_idx].value_ready) return;

    uint64_t pc          = vpq[vpq_idx].pc;
    uint64_t current_ctx = vpq[vpq_idx].value;
    uint32_t vht_idx     = get_index(pc);

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
            if (vpq[p].value_ready) break;

            uint32_t pt_idx = pt_get_index(vht_idx, current_ctx);
            pt_entry_t &e   = pt[pt_idx];

            if (e.conf < fwd_threshold) break;

            vpq[p].fwd_val   = e.value;
            vpq[p].fwd_valid = true;
            current_ctx      = e.value;
        }

        p = (p + 1) % vpq_size;
        steps++;
    }
}

//============================================================
// SVP training
//============================================================
void vpu_t::svp_train(uint64_t pc, uint64_t value) {
    uint32_t idx = get_index(pc);
    uint64_t tag = get_tag(pc);
    svp_entry_t &entry = svp[idx];

    bool hit = (tag_bits == 0 || entry.tag == tag);

    if (hit) {
        int64_t new_stride = (int64_t)value - (int64_t)entry.retired_value;
        if (new_stride == entry.stride) {
            if (entry.conf < conf_max) entry.conf++;
        } else {
            entry.stride = new_stride;
            entry.conf   = 0;
        }
        entry.retired_value = value;
        entry.instance--;
    } else {
        // Replace — walk VPQ for in-flight count
        int64_t in_flight = 0;
        uint32_t i        = (vpq_head + 1) % vpq_size;
        uint32_t to_walk  = (vpq_free_count() == 0) ? (vpq_size - 1) :
                            (vpq_size - vpq_free_count() - 1);
        for (uint32_t n = 0; n < to_walk; n++) {
            if (vpq[i].valid && vpq[i].pc == pc) in_flight++;
            i = (i + 1) % vpq_size;
        }
        entry.valid         = true;
        entry.tag           = tag;
        entry.conf          = 0;
        entry.retired_value = value;
        entry.stride        = (int64_t)value;
        entry.instance      = in_flight;
    }
}

//============================================================
// CVP training — VHT + PT
//============================================================
void vpu_t::cvp_train(uint64_t pc, uint64_t value) {
    uint32_t vht_idx = get_index(pc);
    uint64_t vht_tag = get_tag(pc);

    bool vht_hit = vht[vht_idx].valid &&
                   (tag_bits == 0 || vht[vht_idx].tag == vht_tag);

    if (vht_hit) {
        uint64_t last_value = vht[vht_idx].last_value;
        uint32_t pt_idx     = pt_get_index(vht_idx, last_value);
        pt_entry_t &e       = pt[pt_idx];

        if (e.value == value) {
            if (e.conf < conf_max) e.conf++;
        } else {
            if (e.conf >= conf_miss_pen)
                e.conf -= conf_miss_pen;
            else
                e.conf = 0;
            e.value = value;
        }
    }

    // Update VHT
    vht[vht_idx].valid      = true;
    vht[vht_idx].tag        = vht_tag;
    vht[vht_idx].last_value = value;
}

//============================================================
// Mediator update
//============================================================
void vpu_t::mediator_update(uint64_t pc, bool svp_correct, bool cvp_correct) {
    // uint32_t idx = get_index(pc);
    uint32_t idx = get_mediator_index(pc);
    uint8_t  &c  = mediator[idx];

    // Symmetric rule:
    //   Only move counter when one predictor is clearly better
    if (svp_correct && !cvp_correct) {
        if (c < 3) c++;       // increment toward SVP
    } else if (!svp_correct && cvp_correct) {
        if (c > 0) c--;       // decrement toward CVP
    }
    // Both correct OR both wrong → no change
}

//============================================================
// train() — called at retirement
// Trains both predictors and updates mediator
//============================================================
void vpu_t::train(uint32_t vpq_idx) {
    assert(vpq_idx == vpq_head);
    assert(vpq[vpq_idx].valid);
    assert(vpq[vpq_idx].value_ready);

    uint64_t pc    = vpq[vpq_idx].pc;
    uint64_t value = vpq[vpq_idx].value;

    // Evaluate both predictions against committed value
    bool svp_correct = vpq[vpq_idx].svp_hit &&
                       vpq[vpq_idx].svp_conf &&
                       (vpq[vpq_idx].svp_val == value);
    bool cvp_correct = vpq[vpq_idx].cvp_hit &&
                       vpq[vpq_idx].cvp_conf &&
                       (vpq[vpq_idx].cvp_val == value);

    // Update mediator
    mediator_update(pc, svp_correct, cvp_correct);

    // Train both predictors
    svp_train(pc, value);
    cvp_train(pc, value);

    // Free VPQ head
    vpq[vpq_idx].valid       = false;
    vpq[vpq_idx].value_ready = false;
    vpq_head++;
    if (vpq_head == vpq_size) {
        vpq_head       = 0;
        vpq_head_phase = !vpq_head_phase;
    }
}

//============================================================
// repair_instances() — SVP instance counter rollback
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
            // SVP instance counter rollback (if SVP hit on this entry)
            if (vpq[vpq_tail].svp_hit) {
                uint32_t idx = get_index(vpq[vpq_tail].pc);
                uint64_t tag = get_tag(vpq[vpq_tail].pc);
                svp_entry_t &e = svp[idx];
                if (e.valid && (tag_bits == 0 || e.tag == tag))
                    e.instance--;
            }
            vpq[vpq_tail].valid       = false;
            vpq[vpq_tail].value_ready = false;
            vpq[vpq_tail].fwd_valid   = false;
        }
    }

    assert(vpq_tail       == rollback_tail);
    assert(vpq_tail_phase == rollback_tail_phase);
}

//============================================================
// full_flush()
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
            if (vpq[i].svp_hit) {
                uint32_t idx = get_index(vpq[i].pc);
                uint64_t tag = get_tag(vpq[i].pc);
                svp_entry_t &e = svp[idx];
                if (e.valid && (tag_bits == 0 || e.tag == tag))
                    e.instance--;
            }
            vpq[i].valid       = false;
            vpq[i].value_ready = false;
            vpq[i].fwd_valid   = false;
        }
        i = (i + 1) % vpq_size;
    }

    vpq_tail       = vpq_head;
    vpq_tail_phase = vpq_head_phase;
}