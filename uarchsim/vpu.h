#ifndef VPU_H
#define VPU_H

#include <cinttypes>
#include <cassert>

//////////////////////////
//SVP entry structure
//////////////////////////
struct svp_entry_t {
    uint64_t tag;           // PC tag
    uint64_t conf;          // saturating confidence counter
    uint64_t retired_value; // last actual value seen at retirement
    int64_t  stride;        // signed stride -> +ve/-ve both stride possible
    int64_t  instance;      // speculative in-flight instance counter
};

//////////////////////////
//VPQ entry structure
//////////////////////////
struct vpq_entry_t {
    uint64_t pc;            // full PC of the instruction (TODO: Check if last 2 bits can be discarded)
    uint64_t value;         // actual computed value (filled at execute/writeback)
    //speculative prediction variables
    uint64_t vp_val;        //specutaviely predicted value.
    bool predicted;         //signifies if the value was predicted via SVP
    bool confident;         //signifies if the value was confident. USeful when dispatching

    //TODO: Check if required
    bool valid;         // to check if the entry exists
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

    uint32_t  num_chkpts;
    uint32_t *vpq_checkpoint_tail;
    bool     *vpq_checkpoint_tail_phase;
                            
    uint32_t svp_num_entries; // 2^index_bits

    // SVP table
    svp_entry_t *svp;

    // VPQ circular buffer
    vpq_entry_t *vpq;
    uint32_t vpq_head;
    uint32_t vpq_tail;
    bool vpq_head_phase;
    bool vpq_tail_phase;

    // Constructor / Destructor
    vpu_t(uint32_t vpq_size,
            uint32_t num_chkpts,
            uint32_t index_bits,
            uint32_t tag_bits,
            uint64_t conf_max,
            bool     oracle_conf);
    ~vpu_t();

    //VPQ full/empty checks
    bool full()  { 
        return (vpq_head == vpq_tail) && (vpq_head_phase != vpq_tail_phase); 
    }

    bool empty() { 
        return (vpq_head == vpq_tail) && (vpq_head_phase == vpq_tail_phase); 
    }

    //get the free count of VPQ
    uint32_t vpq_free_count();

    // PC for value prediction and for structures
    // PC bits [1:0]                    -> discard (always 00, RISC-V aligned)
    // PC bits [index_bits+1 : 2]       -> PCindex (used for indexing into SVP table)
    // PC bits [63 : index_bits+2]      -> PCtag   (used to compare tag in SVP table)

    uint64_t get_index(uint64_t pc);
    uint64_t get_tag(uint64_t pc);


    ///////////////////////////////
    // Rename stage
    ///////////////////////////////
    // Returns true if SVP hit
    void predict(uint64_t  pc,              //index into SVP table using PC
                uint32_t vpq_idx,
                uint64_t actual_value = 0); //the actual value from the functional simulator for oracle confidence
    
    // Allocate a VPQ entry at tail for this instruction
    uint32_t vpq_alloc(uint64_t pc);
    void vpq_checkpoint(uint32_t branch_ID);
    void vpq_repair(uint32_t branch_ID);

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
    void repair_instances(uint32_t rollback_tail, bool rollback_tail_phase);
    void full_flush();

    //Compute all the bits required for VPU 
    uint64_t svp_storage_bytes();
};

#endif // VPU_H