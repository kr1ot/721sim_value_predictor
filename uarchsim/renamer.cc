#include "renamer.h"
#include <stdio.h>

renamer::renamer(uint64_t n_log_regs,
		uint64_t n_phys_regs,
		uint64_t n_branches,
		uint64_t n_active)
{
    this->n_branches = n_branches;
    this->n_log_regs = n_log_regs;

    //create rmt with size n_log_regs
    rmt = new uint64_t[n_log_regs];
    //pipeline empty state
    for (uint64_t idx = 0; idx < n_log_regs; idx++){
        rmt[idx] = idx;
    }
    
    //similarly initialize AMT
    amt = new uint64_t[n_log_regs];
    for (uint64_t idx = 0; idx < n_log_regs; idx++){
        amt[idx] = idx;
    }

    //size of free list = n_phys_reg - n_log_regs
    free_list.size = n_phys_regs - n_log_regs;

    //create the free list
    free_list.free_list_entry = new uint64_t[free_list.size];
    for (uint64_t idx = 0; idx < free_list.size; idx++){
        //free list contains all the physical register mappings other than
        //registers in the RMT 
        free_list.free_list_entry[idx] = n_log_regs + idx;
    }

    //initialize free list to be full 
    free_list.head_ptr = 0;
    free_list.tail_ptr = 0;
    free_list.head_phase = false;
    free_list.tail_phase = true;

    //create Active list with size n_active
    active_list.size = n_active;
    active_list.active_list_entry = new active_list_entry_s[active_list.size];
    
    //make active list empty using the pointers
    active_list.head_ptr = 0;
    active_list.tail_ptr = 0;
    active_list.head_phase = false;
    active_list.tail_phase = false;

    //create physical register file with the size n_phys_reg
    prf = new uint64_t[n_phys_regs];
    //same size for the ready bits
    prf_ready_bits = new bool[n_phys_regs];
    //make the logical registers ready
    for (uint64_t idx = 0; idx < n_log_regs; idx++){
        prf_ready_bits[idx] = true;
    }

    //initial state of GBM is all zeros to indicate no branch prior to the instruction
    GBM = 0;

    //create as many branch checkpoints as number of resolved branches
    branch_checkpoint = new branch_checkpoint_s[n_branches];
    //for each branch checkpoint, create shadow map table of depth n_log_regs
    for (uint64_t idx = 0; idx < n_branches; idx++){
        branch_checkpoint[idx].shadow_map_table = new uint64_t[n_log_regs];
    }

}

bool renamer::stall_reg(uint64_t bundle_dst)
{
    //get the number of "free" free list entries
    //way to decode based on head and tail pointer phases -
    //1. same phase flags 
    //  - direct subtraction -> tail - head (since free entries are pushed at tail)
    //2. different phase flags
    //  - subtraction from the size 
    uint64_t free_fl_entries;
    if (free_list.head_phase == free_list.tail_phase)
        free_fl_entries = free_list.tail_ptr - free_list.head_ptr;
    else
        free_fl_entries = free_list.size - (free_list.head_ptr - free_list.tail_ptr);

    return (free_fl_entries < bundle_dst);
}

bool renamer::stall_branch(uint64_t bundle_branch){
    //count number of 1s in the GBM 
    uint64_t unresolve_branch_mask;
    if (n_branches == 64)
        unresolve_branch_mask = ~0ULL;
    else
        unresolve_branch_mask = (1ULL << n_branches) - 1;
    
    uint64_t masked_gbm = unresolve_branch_mask & GBM;
    uint64_t count_ones = 0;
    //find the number of 1s in the masked gbm
    for (uint64_t idx = 0; idx < n_branches; idx++){
        if ((masked_gbm & 1) == 1) {
            count_ones = count_ones + 1;
        }
        masked_gbm = masked_gbm >> 1;
    }
    if ((n_branches - count_ones) >= bundle_branch) return false; 
    else return true;
}

uint64_t renamer::get_branch_mask()
{
    return GBM;
}

uint64_t renamer::rename_rsrc(uint64_t log_reg)
{
    //get the mapping of the src register from RMT
    return rmt[log_reg];
}

uint64_t renamer::rename_rdst(uint64_t log_reg)
{
    //get the entry from head of the free list
    uint64_t prf_index;
    prf_index = free_list.free_list_entry[free_list.head_ptr]; 

    //assign the index to rmt
    rmt[log_reg] = prf_index;

    //increment head ensuring the the phase bits are taken care of
    free_list.head_ptr++;
    if (free_list.head_ptr == free_list.size){
        //roll over
        free_list.head_ptr = 0;
        free_list.head_phase = !free_list.head_phase;
    }
    return prf_index;
}

uint64_t renamer::checkpoint()
{
    //get the location of first 0 from the GBM
    uint64_t branch_id = 0; //will be used as branch ID
    uint64_t gbm_mask = (n_branches == 64) ? ~0ULL : ((1ULL << n_branches)- 1);
    //get the masked gbm with 64 bit vector
    uint64_t masked_gbm = GBM & gbm_mask;
    while(branch_id != n_branches){
        //when got the 1st zero, got a free branch space and can now checkpoint
        if ((masked_gbm & 1) == 0) 
        {
            //checkpoint GBM
            branch_checkpoint[branch_id].checkpoint_GBM = GBM;
            //checkpoint free list pointers
            branch_checkpoint[branch_id].checkpoint_fl_head_ptr = free_list.head_ptr;
            branch_checkpoint[branch_id].checkpoint_fl_head_ptr_phase = free_list.head_phase;
            //checkpoint the entire RMT
            for (uint64_t idx = 0; idx < n_log_regs; idx++)
            {
                branch_checkpoint[branch_id].shadow_map_table[idx] = rmt[idx];
            }
            //update the GBM and make 0 -> 1 to indicate it has been checkpointed
            //and indexed using branch_id or the branch ID
            //printf("Called a branch with GBM = %u\n",GBM);
            GBM = GBM | (1ULL << branch_id);
            //printf("Updated GBM to %u\n",GBM);
            break;
        }

        //if not zero, move to the next "0" spot
        branch_id = branch_id + 1;
        masked_gbm = masked_gbm >> 1;
    }
    //printf("Returned branch ID = %u\n",branch_id);
    return branch_id;
}

bool renamer::stall_dispatch(uint64_t bundle_inst)
{
    //get the number of "free" active list entries
    //way to decode based on head and tail pointer phases -
    //1. same phase flags 
    //  - direct subtraction -> tail - head (since entries are allocated at tail)
    //2. different phase flags
    //  - subtraction from the size 
    uint64_t used_al_entries;
    if (active_list.head_phase == active_list.tail_phase)
        used_al_entries = active_list.tail_ptr - active_list.head_ptr;
    else
        used_al_entries = active_list.size - (active_list.head_ptr - active_list.tail_ptr);
    
    uint64_t free_al_entries = active_list.size - used_al_entries;

    return (free_al_entries < bundle_inst);
}

uint64_t renamer::dispatch_inst(bool dest_valid,
	                       uint64_t log_reg,
	                       uint64_t phys_reg,
	                       bool load,
	                       bool store,
	                       bool branch,
	                       bool amo,
	                       bool csr,
	                       uint64_t PC)
{
    //get the index of the active list from the tail 
    uint64_t active_list_index;
    active_list_index = active_list.tail_ptr;
    //assign the entry at tail
    active_list.active_list_entry[active_list_index].has_dst_reg = dest_valid;
    if (dest_valid == true){
        active_list.active_list_entry[active_list_index].log_reg_num = log_reg;
        active_list.active_list_entry[active_list_index].phy_reg_num = phys_reg;
    }
    else {
        //assigning it 0, anyways wont be using these when dest_valid is false
        active_list.active_list_entry[active_list_index].log_reg_num = 0;
        active_list.active_list_entry[active_list_index].phy_reg_num = 0;
    }
    //assign all the other variables related to execution as false as they havent executed yet
    active_list.active_list_entry[active_list_index].has_exec_completed = false;

    active_list.active_list_entry[active_list_index].is_exception = false;
    active_list.active_list_entry[active_list_index].is_load_violation = false;
    active_list.active_list_entry[active_list_index].is_branch_mispred = false;
    active_list.active_list_entry[active_list_index].is_val_mispred = false;

    //based on the type of instruction, assign the variables
    active_list.active_list_entry[active_list_index].is_instr_load = load;
    active_list.active_list_entry[active_list_index].is_instr_store = store;
    active_list.active_list_entry[active_list_index].is_instr_branch = branch;
    active_list.active_list_entry[active_list_index].is_instr_amo = amo;
    active_list.active_list_entry[active_list_index].is_instr_csr = csr;

    active_list.active_list_entry[active_list_index].pc = PC;

    //increment the tail now
    active_list.tail_ptr++;
    if (active_list.tail_ptr == active_list.size){
        active_list.tail_ptr = 0;
        active_list.tail_phase = !active_list.tail_phase;
    }

    //return the index from active list where the instruction is assigned
    return active_list_index;
}

bool renamer::is_ready(uint64_t phys_reg)
{
    return prf_ready_bits[phys_reg];
}

void renamer::clear_ready(uint64_t phys_reg)
{
    prf_ready_bits[phys_reg] = false;
}

uint64_t renamer::read(uint64_t phys_reg)
{
    return prf[phys_reg];
}

void renamer::set_ready(uint64_t phys_reg)
{
    prf_ready_bits[phys_reg] = true;
}

void renamer::write(uint64_t phys_reg, uint64_t value)
{
    prf[phys_reg] = value;
}

void renamer::set_complete(uint64_t AL_index)
{
    active_list.active_list_entry[AL_index].has_exec_completed = true;
}

void renamer::resolve(uint64_t AL_index,
		     uint64_t branch_ID,
		     bool correct)
{
    //if branch is correctly predicted, no recovery required.
    //clear branch bit in the GBM and all checkpointed GBMs
    if (correct == true)
    {
        //printf("**Resolving branch ID = %u\n",branch_ID);
        //printf("**Before resolution GBM = %u\n",GBM);
        GBM &= ~(1ULL << branch_ID);
        //printf("**After resolution GBM = %u\n",GBM);

        //loop through all the checkpoints and reset them to 0
        for (uint64_t idx = 0; idx < n_branches; idx++)
        {
            branch_checkpoint[idx].checkpoint_GBM &= ~(1ULL << branch_ID);
        }
    }
    else 
    {
        //update the GBM to checkpointed GBM based on branch ID
        //and clear the bit from the checkpointed GBM
        GBM = branch_checkpoint[branch_ID].checkpoint_GBM & ~(1ULL << branch_ID);
        //restore RMT from the shadow map table
        for (uint64_t idx = 0; idx < n_log_regs; idx++)
        {
            rmt[idx] = branch_checkpoint[branch_ID].shadow_map_table[idx];
        }
        free_list.head_ptr = branch_checkpoint[branch_ID].checkpoint_fl_head_ptr;
        free_list.head_phase = branch_checkpoint[branch_ID].checkpoint_fl_head_ptr_phase;

        //restore the active list tail pointer from AL_index. Its the next index after
        //branch
        active_list.tail_ptr = (AL_index + 1) % active_list.size;
        //retrieve the phase based on the position of tail pointer with respect to head
        //if tail is ahead, then same phase. it cannot be equal as AL is not empty
        if (active_list.tail_ptr > active_list.head_ptr) active_list.tail_phase = active_list.head_phase;
        //if tail is behind, then phase is different
        else active_list.tail_phase = !active_list.head_phase;

    }
}

bool renamer::precommit(bool &completed,
                       bool &exception, bool &load_viol, bool &br_misp, bool &val_misp,
	               bool &load, bool &store, bool &branch, bool &amo, bool &csr,
		       uint64_t &PC)
{
    //check if active list is empty
    if ((active_list.head_ptr == active_list.tail_ptr) &&
        (active_list.head_phase == active_list.tail_phase)) { return false;}
    
    //peek for all the required parameters
    completed = active_list.active_list_entry[active_list.head_ptr].has_exec_completed;
    exception = active_list.active_list_entry[active_list.head_ptr].is_exception;
    load_viol = active_list.active_list_entry[active_list.head_ptr].is_load_violation;
    br_misp = active_list.active_list_entry[active_list.head_ptr].is_branch_mispred;
    val_misp = active_list.active_list_entry[active_list.head_ptr].is_val_mispred;
    load = active_list.active_list_entry[active_list.head_ptr].is_instr_load;
    store = active_list.active_list_entry[active_list.head_ptr].is_instr_store;
    branch = active_list.active_list_entry[active_list.head_ptr].is_instr_branch;
    amo = active_list.active_list_entry[active_list.head_ptr].is_instr_amo;
    csr = active_list.active_list_entry[active_list.head_ptr].is_instr_csr;
    PC = active_list.active_list_entry[active_list.head_ptr].pc;
    
    return true;

}

void renamer::commit()
{
    //if destination exists for the instruction at head
    // 1. get the log and phys reg 
    // 2. index the AMT using log reg and get the mapping
    // 3. store the mapping at tail of free list
    // 4. replace the index in AMT with phys reg
    //if destination does not exist for instruction
    // - do nothing
    if (active_list.active_list_entry[active_list.head_ptr].has_dst_reg == true)
    {
        uint64_t log_reg = active_list.active_list_entry[active_list.head_ptr].log_reg_num;
        uint64_t phys_reg = active_list.active_list_entry[active_list.head_ptr].phy_reg_num;

        free_list.free_list_entry[free_list.tail_ptr] = amt[log_reg];
        //increment tail
        free_list.tail_ptr++;
        if (free_list.tail_ptr == free_list.size)
        {
            free_list.tail_ptr = 0;
            free_list.tail_phase = !free_list.tail_phase;
        }
        //update amt
        amt[log_reg] = phys_reg; 
    }

    //commited and freed the registers, now
    //increment the head pointer in active list
    active_list.head_ptr++;
    if (active_list.head_ptr == active_list.size)
    {
        active_list.head_ptr = 0;
        active_list.head_phase = !active_list.head_phase;
    }
}

void renamer::squash()
{
    //squash all the instructions
    //1.flash copy amt into rmt
    for (uint64_t idx = 0; idx < n_log_regs; idx++)
        rmt[idx] = amt[idx];
    //2. restore the active list to empty
    active_list.tail_ptr = active_list.head_ptr;
    active_list.tail_phase = active_list.head_phase;
    //3. restore the GBM checkpoints to all free
    GBM = 0ULL;
    //4. make free list full
    free_list.head_ptr = free_list.tail_ptr;
    free_list.head_phase = !free_list.tail_phase;
}

void renamer::set_exception(uint64_t AL_index)
{
    active_list.active_list_entry[AL_index].is_exception = true;
}

void renamer::set_load_violation(uint64_t AL_index)
{
    active_list.active_list_entry[AL_index].is_load_violation = true;
}

void renamer::set_branch_misprediction(uint64_t AL_index)
{
    active_list.active_list_entry[AL_index].is_branch_mispred = true;
}

void renamer::set_value_misprediction(uint64_t AL_index)
{
    active_list.active_list_entry[AL_index].is_val_mispred = true;
}

bool renamer::get_exception(uint64_t AL_index)
{
    return active_list.active_list_entry[AL_index].is_exception;
}

