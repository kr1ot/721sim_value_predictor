#ifndef VPU_H
#define VPU_H

#include <cinttypes>
#include <cassert>
#include <cstring>
#include <map>
#include <utility>

//////////////////////////////////////////////////////////////
// CVP Prediction Table entry
//////////////////////////////////////////////////////////////
typedef struct {
    uint64_t value;
    uint32_t conf;
} cvp_pred_entry_t;

//////////////////////////////////////////////////////////////
// VPQ Entry
//////////////////////////////////////////////////////////////
typedef struct {
    uint64_t pc;
    bool     valid;

    // Real computed value (set at execute)
    uint64_t value;
    bool     value_ready;

    // Rename-time prediction (used for misprediction check at execute)
    uint64_t vp_val;
    bool     predicted;
    bool     confident;

    // Forward-walk deposited context (used by next instance's backward walk)
    uint64_t fwd_val;
    bool     fwd_valid;

    // Misc
    unsigned int flags;
} vpq_entry_t;

//////////////////////////////////////////////////////////////
// VPU — CVP Order-1
//////////////////////////////////////////////////////////////
class vpu_t {
public:
    //-----------------------------
    // Config
    //-----------------------------
    uint32_t vpq_size;
    uint32_t conf_max;         // confidence threshold for prediction
    uint32_t conf_miss_pen;    // decrement on misprediction
    bool     oracle_conf;

    //-----------------------------
    // CVP tables (infinite — std::map)
    //-----------------------------
    // VHT: PC → last committed value
    std::map<uint64_t, uint64_t> vht;

    // Prediction table: (PC, last_value) → {predicted_next_value, confidence}
    std::map<std::pair<uint64_t, uint64_t>, cvp_pred_entry_t> pred_table;

    //-----------------------------
    // VPQ (circular buffer with phase bits)
    //-----------------------------
    vpq_entry_t *vpq;
    uint32_t     vpq_head;
    uint32_t     vpq_tail;
    bool         vpq_head_phase;
    bool         vpq_tail_phase;

    //-----------------------------
    // VPQ checkpoints
    //-----------------------------
    uint32_t  num_chkpts;
    uint32_t *vpq_checkpoint_tail;
    bool     *vpq_checkpoint_tail_phase;

    //-----------------------------
    // Constructor / Destructor
    //-----------------------------
    vpu_t(uint32_t vpq_size,
          uint32_t num_chkpts,
          uint32_t conf_max,
          uint32_t conf_miss_pen,
          bool     oracle_conf);
    ~vpu_t();

    //-----------------------------
    // VPQ helpers (unchanged)
    //-----------------------------
    bool     full()  { return (vpq_head == vpq_tail) &&
                              (vpq_head_phase != vpq_tail_phase); }
    bool     empty() { return (vpq_head == vpq_tail) &&
                              (vpq_head_phase == vpq_tail_phase); }
    uint32_t vpq_free_count() {
        if (vpq_tail_phase == vpq_head_phase)
            return vpq_size - (vpq_tail - vpq_head);
        else
            return vpq_head - vpq_tail;
    }

    //-----------------------------
    // VPQ operations
    //-----------------------------
    uint32_t vpq_alloc(uint64_t pc);
    void     vpq_write_value(uint32_t vpq_idx, uint64_t value);
    void     vpq_checkpoint(uint32_t branch_ID);
    void     vpq_repair(uint32_t branch_ID);

    //-----------------------------
    // Core prediction / training
    //-----------------------------
    // predict(): called at rename — backward walk to find context, look up pred table
    void predict(uint64_t pc, uint32_t vpq_idx, uint64_t actual_value);

    // forward_walk(): called at execute after value_ready set
    //   - starts from (vpq_idx+1)
    //   - deposits fwd_val into matching-PC entries while confidence holds
    void forward_walk(uint32_t vpq_idx);

    // train(): called at retirement — update VHT and prediction table
    void train(uint32_t vpq_idx);

    //-----------------------------
    // Recovery
    //-----------------------------
    void repair_instances(uint32_t rollback_tail, bool rollback_tail_phase);
    void full_flush();

    //-----------------------------
    // Storage cost — prediction table only (VHT excluded per spec)
    //-----------------------------
    uint64_t pred_table_entries() { return pred_table.size(); }
    // Cost is infinite in this implementation — for reporting only

private:
    // Helper: backward walk from just-before tail, find most recent prior
    // entry for same PC. Returns context value and whether valid context found.
    bool backward_walk_find_context(uint64_t pc, uint32_t new_vpq_idx,uint64_t &ctx);
};

#endif // VPU_H