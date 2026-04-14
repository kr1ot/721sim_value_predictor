#ifndef VPU_H
#define VPU_H

#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <cstring>

//////////////////////////
//SVP entry structure
//////////////////////////
struct svp_entry_t {
    uint64_t tag;           // PC tag
    uint64_t conf;          // saturating confidence counter
    uint64_t retired_value; // last actual value seen at retirement
    int64_t  stride;        // signed stride -> +ve/-ve both stride possible
    int64_t  instance;      // speculative in-flight instance counter
    bool     valid;         // to check if the entry exists
};

//////////////////////////
//VPQ entry structure
//////////////////////////
struct vpq_entry_t {
    uint64_t pc;            // full PC of the instruction (TODO: Check if last 2 bits can be discarded)
    uint64_t value;         // actual computed value (filled at execute/writeback)
    bool     value_ready;   // true when execute writes the value
    bool     valid;         // to check if the entry exists
};

//VPU class taking care of entire value prediction
class vpu_t {
public:
    // Configuration
    uint32_t vpq_size;       // number of VPQ entries
    uint32_t index_bits;     // number of SVP index bits. 2**index dictates size of svp
    uint32_t tag_bits;       // number of SVP tag bits (0 = no tag)
    uint64_t conf_max;       // confidence threshold
    bool     oracle_conf;    // true = oracle confidence mode
                            
    uint32_t svp_num_entries; // 2^index_bits

    // SVP table
    svp_entry_t *svp;

    // VPQ circular buffer
    vpq_entry_t *vpq;
    uint32_t vpq_head;
    uint32_t vpq_tail;
    uint32_t vpq_count;    //counts the number of used entries in the vpq buffer

    // Constructor / Destructor
    vpu_t(uint32_t vpq_size,
            uint32_t index_bits,
            uint32_t tag_bits,
            uint64_t conf_max,
            bool     oracle_conf);
    ~vpu_t();

    // PC for value prediction and for structures
    // PC bits [1:0]                    -> discard (always 00, RISC-V aligned)
    // PC bits [index_bits+1 : 2]       -> PCindex (used for indexing into SVP table)
    // PC bits [63 : index_bits+2]      -> PCtag   (used to compare tag in SVP table)

    uint64_t get_index(uint64_t pc);
    uint64_t get_tag(uint64_t pc);

    //VPQ functions for checking the free entries
    bool     vpq_has_free();
    uint32_t vpq_free_count();

    ///////////////////////////////
    // Rename stage
    ///////////////////////////////
    // Returns true if SVP hit
    bool predict(uint64_t  pc,              //index into SVP table using PC
                uint64_t &pred_value,       //get the prediction value
                bool     &confident,        //decide the confidence 
                uint32_t &vpq_tail_out,    //get the tail entry
                uint64_t actual_value = 0); //the actual value from the functional simulator for oracle confidence
    
    // Allocate a VPQ entry at tail for this instruction
    uint32_t vpq_alloc(uint64_t pc);

    ///////////////////////////////
    // Execute/WB stage
    ///////////////////////////////
    // Write actual computed value into VPQ entry
    void vpq_write_value(uint32_t vpq_idx, uint64_t value);

    ///////////////////////////////
    // Retire stage
    ///////////////////////////////
    // Train SVP using VPQ head entry, then free it
    void train(uint32_t vpq_idx);

    ///////////////////////////////
    // Recovery logic
    ///////////////////////////////
    // Walk VPQ backward from tail to rollback_tail,
    // repairing SVP instance counters for squashed instructions
    //TODO: Check properly the rollback mechanism. Which to implement?
    void repair_instances(uint32_t rollback_tail);
    void full_flush();

    //Compute all the bits required for VPU 
    uint64_t svp_storage_bytes();
};

#endif // VPU_H