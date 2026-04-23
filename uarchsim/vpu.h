#ifndef VPU_H
#define VPU_H

#include <cinttypes>
#include <cassert>
#include <cstring>
#include <cmath>

//////////////////////////////////////////////////////////////
// SVP entry (original stride predictor)
//////////////////////////////////////////////////////////////
typedef struct {
    bool     valid;
    uint64_t tag;
    uint32_t conf;
    uint64_t retired_value;
    int64_t  stride;
    int64_t  instance;
} svp_entry_t;

//////////////////////////////////////////////////////////////
// VHT entry (CVP — last value per PC)
//////////////////////////////////////////////////////////////
typedef struct {
    bool     valid;
    uint64_t tag;
    uint64_t last_value;
} vht_entry_t;

//////////////////////////////////////////////////////////////
// PT entry (CVP prediction table)
//////////////////////////////////////////////////////////////
typedef struct {
    uint64_t value;
    uint32_t conf;
} pt_entry_t;

//////////////////////////////////////////////////////////////
// VPQ Entry — holds both SVP and CVP predictions for mediator
//////////////////////////////////////////////////////////////
typedef struct {
    uint64_t pc;
    bool     valid;

    // Actual computed value
    uint64_t value;
    bool     value_ready;

    // What was actually injected into PRF
    uint64_t vp_val;
    bool     predicted;
    bool     confident;

    // Forward-walk deposited context (for CVP)
    uint64_t fwd_val;
    bool     fwd_valid;

    // Mediator tracking — save both predictions at rename time
    uint64_t svp_val;
    bool     svp_hit;
    bool     svp_conf;
    uint64_t cvp_val;
    bool     cvp_hit;
    bool     cvp_conf;
    bool     used_svp;      // true = SVP injected, false = CVP injected

} vpq_entry_t;

//////////////////////////////////////////////////////////////
// VPU — Hybrid SVP + CVP with Per-PC Mediator
//////////////////////////////////////////////////////////////
class vpu_t {
public:
    //-----------------------------
    // Config
    //-----------------------------
    uint32_t vpq_size;
    uint32_t index_bits;
    uint32_t tag_bits;
    uint32_t pt_index_bits;
    uint32_t conf_max;
    uint32_t conf_miss_pen;
    bool     oracle_conf;

    //-----------------------------
    // SVP
    //-----------------------------
    svp_entry_t *svp;
    uint32_t     svp_num_entries;

    //-----------------------------
    // CVP — VHT + PT (finite)
    //-----------------------------
    vht_entry_t *vht;
    uint32_t     vht_num_entries;

    pt_entry_t  *pt;
    uint32_t     pt_num_entries;

    //-----------------------------
    // Mediator — per-PC 2-bit counter
    // Indexed by same bits as VHT (index_bits)
    // 0,1 → use CVP; 2,3 → use SVP; initial = 2
    //-----------------------------
    uint8_t  *mediator;
    uint32_t  mediator_num_entries;

    //-----------------------------
    // VPQ
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
    // Non-speculative commit BHR
    // Updated only at retirement of branch instructions
    //-----------------------------
    uint64_t commit_bhr;
    uint32_t bhr_length;    // how many branch history bits to use
    uint64_t bhr_mask;      // (1 << bhr_length) - 1

    // Called from retire.cc when a branch retires
    void update_commit_bhr(bool taken);
    uint32_t get_mediator_index(uint64_t pc);

    //-----------------------------
    // Constructor / Destructor
    //-----------------------------
    vpu_t(uint32_t vpq_size,
          uint32_t num_chkpts,
          uint32_t index_bits,
          uint32_t tag_bits,
          uint32_t conf_max,
          uint32_t conf_miss_pen,
          bool     oracle_conf,
          uint32_t bhr_length);
    ~vpu_t();

    //-----------------------------
    // VPQ helpers
    //-----------------------------
    bool full()  { return (vpq_head == vpq_tail) &&
                          (vpq_head_phase != vpq_tail_phase); }
    bool empty() { return (vpq_head == vpq_tail) &&
                          (vpq_head_phase == vpq_tail_phase); }
    uint32_t vpq_free_count() {
        if (vpq_tail_phase == vpq_head_phase)
            return vpq_size - (vpq_tail - vpq_head);
        else
            return vpq_head - vpq_tail;
    }

    //-----------------------------
    // Index / tag helpers
    //-----------------------------
    uint32_t get_index(uint64_t pc);       // VHT / SVP / mediator index
    uint64_t get_tag(uint64_t pc);
    uint32_t pt_get_index(uint32_t idx, uint64_t last_value);

    //-----------------------------
    // VPQ operations
    //-----------------------------
    uint32_t vpq_alloc(uint64_t pc);
    void     vpq_write_value(uint32_t vpq_idx, uint64_t value);
    void     vpq_checkpoint(uint32_t branch_ID);
    void     vpq_repair(uint32_t branch_ID);

    //-----------------------------
    // Core predict / train
    //-----------------------------
    void predict(uint64_t pc, uint32_t vpq_idx, uint64_t actual_value);
    void forward_walk(uint32_t vpq_idx);
    void train(uint32_t vpq_idx);

    //-----------------------------
    // Recovery
    //-----------------------------
    void repair_instances(uint32_t rollback_tail, bool rollback_tail_phase);
    void full_flush();

    //-----------------------------
    // Storage cost
    //-----------------------------
    uint32_t get_conf_bits() {
        return (uint32_t)ceil(log2((double)(conf_max + 1)));
    }
    uint32_t get_instance_bits() {
        return (uint32_t)ceil(log2((double)vpq_size));
    }
    uint32_t get_svp_bits_per_entry() {
        // valid + tag + conf + retired_value + stride + instance
        return 1 + tag_bits + get_conf_bits() + 64 + 64 + get_instance_bits();
    }
    uint32_t get_vht_bits_per_entry() {
        return 1 + tag_bits + 64;
    }
    uint32_t get_pt_bits_per_entry() {
        return 64 + get_conf_bits();
    }
    uint64_t get_svp_total_bits() {
        return (uint64_t)get_svp_bits_per_entry() * svp_num_entries;
    }
    uint64_t get_vht_total_bits() {
        return (uint64_t)get_vht_bits_per_entry() * vht_num_entries;
    }
    uint64_t get_pt_total_bits() {
        return (uint64_t)get_pt_bits_per_entry() * pt_num_entries;
    }
    uint64_t get_mediator_total_bits() {
        return 2ULL * mediator_num_entries;
    }
    uint64_t get_total_bits() {
        return get_svp_total_bits() + get_vht_total_bits() +
               get_pt_total_bits() + get_mediator_total_bits();
    }
    uint64_t storage_bytes() {
        return (get_total_bits() + 7) / 8;
    }

private:
    //-----------------------------
    // Internal helpers
    //-----------------------------
    bool backward_walk_find_context(uint64_t pc, uint32_t new_vpq_idx,
                                     uint64_t &ctx);

    // SVP lookup at predict time — speculatively advances instance counter
    void svp_lookup(uint64_t pc, uint64_t actual_value,
                    uint64_t &pred_val, bool &hit, bool &confident);

    // CVP lookup at predict time
    void cvp_lookup(uint64_t pc, uint32_t vpq_idx, uint64_t actual_value,
                    uint64_t &pred_val, bool &hit, bool &confident);

    // Individual trainers
    void svp_train(uint64_t pc, uint64_t value);
    void cvp_train(uint64_t pc, uint64_t value);

    // Mediator update
    void mediator_update(uint64_t pc, bool svp_correct, bool cvp_correct);
};

#endif // VPU_H