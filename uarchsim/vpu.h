#ifndef VPU_H
#define VPU_H

#include <cinttypes>
#include <cassert>
#include <cstring>
#include <cmath>

//////////////////////////////////////////////////////////////
// VHT entry — indexed by PC, tag validation
//////////////////////////////////////////////////////////////
typedef struct {
    bool     valid;
    uint64_t tag;
    uint64_t last_value;   // last committed value for this PC
} vht_entry_t;

//////////////////////////////////////////////////////////////
// Prediction Table entry — indexed by hash(vht_idx, last_value)
// No tag — aliasing handled by confidence counter
//////////////////////////////////////////////////////////////
typedef struct {
    uint64_t value;
    uint32_t conf;
} pt_entry_t;

//////////////////////////////////////////////////////////////
// VPQ Entry
//////////////////////////////////////////////////////////////
typedef struct {
    uint64_t pc;
    bool     valid;
    uint64_t value;
    bool     value_ready;
    uint64_t vp_val;
    bool     predicted;
    bool     confident;
    uint64_t fwd_val;
    bool     fwd_valid;
    unsigned int flags;
} vpq_entry_t;

//////////////////////////////////////////////////////////////
// VPU — Finite CVP Order-1
//////////////////////////////////////////////////////////////
class vpu_t {
public:
    //-----------------------------
    // Config
    //-----------------------------
    uint32_t vpq_size;
    uint32_t index_bits;       // VHT index bits → VHT has 2^index_bits entries
    uint32_t tag_bits;         // VHT tag bits
    uint32_t pt_index_bits;    // PT index bits = index_bits + 1 (2x VHT)
    uint32_t conf_max;
    uint32_t conf_miss_pen;
    bool     oracle_conf;

    //-----------------------------
    // VHT — finite, tag-validated
    //-----------------------------
    vht_entry_t *vht;
    uint32_t     vht_num_entries;

    //-----------------------------
    // Prediction Table — 2x VHT, no tag
    //-----------------------------
    pt_entry_t  *pt;
    uint32_t     pt_num_entries;

    //-----------------------------
    // VPQ (circular buffer)
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
          uint32_t index_bits,
          uint32_t tag_bits,
          uint32_t conf_max,
          uint32_t conf_miss_pen,
          bool     oracle_conf);
    ~vpu_t();

    //-----------------------------
    // VPQ helpers
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
    // VHT / PT index and tag
    //-----------------------------
    uint32_t vht_get_index(uint64_t pc);
    uint64_t vht_get_tag(uint64_t pc);
    uint32_t pt_get_index(uint32_t vht_idx, uint64_t last_value);

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
    uint32_t get_vht_bits_per_entry() {
        return 1 + tag_bits + 64;  // valid + tag + value
    }
    uint32_t get_pt_bits_per_entry() {
        return 64 + get_conf_bits();  // value + conf
    }
    uint64_t get_total_bits() {
        return (uint64_t)get_vht_bits_per_entry() * vht_num_entries +
               (uint64_t)get_pt_bits_per_entry()  * pt_num_entries;
    }
    uint64_t storage_bytes() {
        return (get_total_bits() + 7) / 8;
    }

private:
    bool backward_walk_find_context(uint64_t pc, uint32_t new_vpq_idx,
                                     uint64_t &ctx);
};

#endif // VPU_H