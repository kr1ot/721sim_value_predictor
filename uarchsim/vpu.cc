#include "vpu.h"
#include <cstring>
#include <cstdio>

//============================================================
// Constructor / Destructor
//============================================================
vpu_t::vpu_t(uint32_t vpq_size, uint32_t num_chkpts,
             uint32_t conf_max, uint32_t conf_miss_pen,
             bool oracle_conf) {

    this->vpq_size      = vpq_size;
    this->num_chkpts    = num_chkpts;
    this->conf_max      = conf_max;
    this->conf_miss_pen = conf_miss_pen;
    this->oracle_conf   = oracle_conf;

    // VPQ init
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

    // Checkpoint arrays
    vpq_checkpoint_tail       = new uint32_t[num_chkpts];
    vpq_checkpoint_tail_phase = new bool[num_chkpts];
    memset(vpq_checkpoint_tail,       0, sizeof(uint32_t) * num_chkpts);
    memset(vpq_checkpoint_tail_phase, 0, sizeof(bool)     * num_chkpts);

    // vht and pred_table auto-initialize as empty maps
}

vpu_t::~vpu_t() {
    delete[] vpq;
    delete[] vpq_checkpoint_tail;
    delete[] vpq_checkpoint_tail_phase;
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
// Backward walk helper
// From just-before tail walking toward head, find most recent prior
// VPQ entry for this PC. Use actual value if executed, else fwd_val if valid.
//============================================================
bool vpu_t::backward_walk_find_context(uint64_t pc, uint32_t new_vpq_idx,uint64_t &ctx) {
    // Number of entries in-flight (excluding the new entry being allocated)
    uint32_t entries_in_flight;
    if (vpq_tail_phase == vpq_head_phase)
        entries_in_flight = vpq_tail - vpq_head;
    else
        entries_in_flight = vpq_size - vpq_head + vpq_tail;

    // Walk backward from tail-1 down to head
    for (uint32_t n = 0; n < entries_in_flight; n++) {
        uint32_t pos = (vpq_tail + vpq_size - 1 - n) % vpq_size;

        if (!vpq[pos].valid) continue;
        if (pos == new_vpq_idx)     continue;   // skip new entry
        if (vpq[pos].pc != pc) continue;

        // Found most recent prior instance of this PC
        // Priority: actual value > forward-walked predicted value
        if (vpq[pos].value_ready) {
            ctx = vpq[pos].value;
            return true;
        }
        if (vpq[pos].fwd_valid) {
            ctx = vpq[pos].fwd_val;
            return true;
        }
        // Prior instance has no usable context — do not propagate further
        return false;
    }

    // No prior in-flight instance — use committed VHT
    auto it = vht.find(pc);
    if (it != vht.end()) {
        ctx = it->second;
        return true;
    }

    return false;
}

//============================================================
// predict() — called at rename
// Backward walk to find context, look up prediction table
//============================================================
void vpu_t::predict(uint64_t pc, uint32_t vpq_idx,
                    uint64_t actual_value) {

    // Step 1: find context via backward walk
    uint64_t ctx;
    bool     have_ctx = backward_walk_find_context(pc, vpq_idx,ctx);

    //  fprintf(stderr, "predict: PC=0x%lx have_ctx=%d ctx=%lu "
    //                 "table_size=%lu\n",
    //         pc, have_ctx, ctx, pred_table.size());

    if (!have_ctx) {
        // No context available — cannot predict
        vpq[vpq_idx].predicted = false;
        vpq[vpq_idx].confident = false;
        vpq[vpq_idx].vp_val    = 0;
        return;
    }

    // Step 2: look up prediction table
    auto key = std::make_pair(pc, ctx);
    auto it  = pred_table.find(key);
    // fprintf(stderr, "predict: key lookup hit=%d\n",
    //         it != pred_table.end());
    if (it == pred_table.end()) {
        // No prediction table entry for this context
        vpq[vpq_idx].predicted = false;
        vpq[vpq_idx].confident = false;
        vpq[vpq_idx].vp_val    = 0;
        return;
    }

    // Step 3: check confidence
    bool confident;
    if (oracle_conf)
        confident = (it->second.value == actual_value);
    else
        confident = (it->second.conf >= conf_max);

    vpq[vpq_idx].predicted = true;
    vpq[vpq_idx].vp_val    = it->second.value;
    vpq[vpq_idx].confident = confident;
}

//============================================================
// forward_walk() — called at execute after vpq_write_value()
// Start from (vpq_idx+1), propagate predictions to matching-PC entries
// Stop conditions:
//   1. Reached tail
//   2. Found another entry with value_ready=true (its own execute will propagate)
//   3. Prediction table miss or low confidence → chain broken
//============================================================
void vpu_t::forward_walk(uint32_t vpq_idx) {
    if (!vpq[vpq_idx].valid) return;
    if (!vpq[vpq_idx].value_ready) return;   // must have a real value to seed

    uint64_t pc          = vpq[vpq_idx].pc;
    uint64_t current_ctx = vpq[vpq_idx].value;

    // Walk forward from vpq_idx+1 until tail
    uint32_t pos       = (vpq_idx + 1) % vpq_size;
    bool     pos_phase = (pos == 0 && vpq_idx == vpq_size - 1)
                         ? !vpq_head_phase    // approximation; see note below
                         : false;

    // Compute stopping condition using position vs tail
    // Simpler: walk at most vpq_size steps, stop when pos == vpq_tail
    uint32_t steps = 0;
    uint32_t p     = (vpq_idx + 1) % vpq_size;

    // Handle phase: if vpq_idx+1 wrapped to 0, we are in tail_phase (same as tail)
    // Actually simpler — just check p != vpq_tail OR phase mismatch; bounded by vpq_size
    while (steps < vpq_size) {
        // Stop if reached tail
        // To handle wrap correctly, check if we have walked as many entries as
        // exist between vpq_idx+1 and vpq_tail
        if (p == vpq_tail) {
            // Need to also check phase — but for forward walk from an entry
            // that was already allocated (valid), p reaching tail means we
            // walked past all in-flight entries after vpq_idx
            break;
        }

        if (!vpq[p].valid) {
            // Should not happen in a well-formed VPQ, but be defensive
            p = (p + 1) % vpq_size;
            steps++;
            continue;
        }

        if (vpq[p].pc == pc) {
            // Matching PC found

            // Stop if this instance already has real value — its own
            // forward walk will propagate from there
            if (vpq[p].value_ready)
                break;

            // Look up prediction table with current context
            auto key = std::make_pair(pc, current_ctx);
            auto it  = pred_table.find(key);
            if (it == pred_table.end()) {
                break;   // no entry — chain broken
            }

            uint32_t fwd_threshold = conf_max / 2;
            if (fwd_threshold == 0) fwd_threshold = 1;  // guard against conf_max=1 edge case

            if (it->second.conf < fwd_threshold) {
                break;
            }

            // Deposit prediction
            vpq[p].fwd_val   = it->second.value;
            vpq[p].fwd_valid = true;

            // Update context for next iteration
            current_ctx = it->second.value;
        }

        p = (p + 1) % vpq_size;
        steps++;
    }
}

//============================================================
// train() — called at retirement
// Update prediction table using (PC, last_value_from_VHT) → committed_value
// Update VHT[PC] = committed_value
//============================================================
void vpu_t::train(uint32_t vpq_idx) {
    assert(vpq_idx == vpq_head);
    assert(vpq[vpq_idx].valid);
    assert(vpq[vpq_idx].value_ready);

    uint64_t pc    = vpq[vpq_idx].pc;
    uint64_t value = vpq[vpq_idx].value;

    // Step 1: if VHT has a prior value, update prediction table
    auto vht_it = vht.find(pc);
    if (vht_it != vht.end()) {
        uint64_t last_value = vht_it->second;
        auto key    = std::make_pair(pc, last_value);
        auto tbl_it = pred_table.find(key);

        //  fprintf(stderr, "train: PC=0x%lx last_val=%lu new_val=%lu "
        //                 "table_size=%lu ",
        //         pc, last_value, value, pred_table.size());

        if (tbl_it == pred_table.end()) {
            // Allocate new entry
            cvp_pred_entry_t entry;
            entry.value      = value;
            entry.conf       = 0;
            pred_table[key]  = entry;
        } else {
            if (tbl_it->second.value == value) {
                // Correct — increment confidence
                if (tbl_it->second.conf < conf_max)
                    tbl_it->second.conf++;
            } else {
                // Wrong — apply miss penalty and update value
                if (tbl_it->second.conf >= conf_miss_pen)
                    tbl_it->second.conf -= conf_miss_pen;
                else
                    tbl_it->second.conf = 0;
                tbl_it->second.value = value;
            }
        }
    }

    // Step 2: update VHT (always)
    vht[pc] = value;

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
// repair_instances() — branch mispredict rollback
// Walk tail backward, freeing entries. No special CVP state to restore
// because VHT and prediction table are non-speculative.
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
// full_flush() — complete squash (value misprediction or exception)
// Empty the entire VPQ. VHT and prediction table survive.
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