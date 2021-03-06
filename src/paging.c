#include "common.h"
#include "kheap.h"
#include "paging.h"
#include "monitor.h"
#define INDEX_FROM_BIT(address) (address/(8*4))
#define OFFSET_FROM_BIT(address) (address%(8*4))
u32int *frames;
u32int nframes;
extern u32int placement_address; //0x10e00c
extern heap_t* kheap;
page_directory_t* current_directory = 0;
page_directory_t* kernel_directory = 0;

static void set_frame(u32int frame_address){
    u32int frame = frame_address/0x1000; // total 2^32 / 4096 = 1048576 frames
    u32int idx = INDEX_FROM_BIT(frame); //1048576 / 32 = 32768 idxs in total
    u32int off = OFFSET_FROM_BIT(frame);//log(32) = 5 = 5 bits for offset per idx
    frames[idx] |= (0x1 << off);
}

static void clear_frame(u32int frame_address){
    u32int frame = frame_address/0x1000; // total 2^32 / 4096 = 1048576 frames
    u32int idx = INDEX_FROM_BIT(frame); //1048576 / 32 = 32768 idxs in total
    u32int off = OFFSET_FROM_BIT(frame);//log(32) = 5 = 5 bits for offset per idx
    frames[idx] &= ~(0x1 << off);
}

static u32int test_frame(u32int frame_address){
    u32int frame = frame_address/0x1000; // total 2^32 / 4096 = 1048576 frames
    u32int idx = INDEX_FROM_BIT(frame); //1048576 / 32 = 32768 idxs in total
    u32int off = OFFSET_FROM_BIT(frame);//log(32) = 5 = 5 bits for offset per idx
    return (frames[idx] & (0x1 << off));

}

static u32int first_frame(){
    u32int i, j;
    for (int i = 0; i < INDEX_FROM_BIT(nframes); i++){
        if (frames[i] != 0xFFFFFFFF){
            for (j = 0; j < 32; j++){
                u32int toTest = 0x1 << j;
                if(!(frames[i]&toTest)){
                    return i*4*8+j;
                }
            }
        }
    }
} 

void alloc_frame(page_t *page, int is_kernel, int is_writable){
    if (page->frame != 0){
        return;
    } else {
        u32int idx = first_frame();
        if(idx == (u32int) - 1){
        }
        set_frame(idx*0x1000); //This frame is now ours.
        page->present = 1;
        page->rw = (is_writable)? 1 : 0; 
        page->user = (is_kernel)? 0 : 1;
        page->frame = idx;
    }
}

void free_frame(page_t *page){
    u32int frame;
    if (!(frame = page->frame)){
        return; //This given page didn't actually have an allocated frame.
    }
    else{
        clear_frame(frame); //Frame is now free again.
        page->frame = 0x0; //Page now doesn't have a frame.
    }
}

void initialize_paging(){
    u32int mem_end_page = 0x1000000; //Assume the physical memory is 16 MB 
    nframes = mem_end_page / 0x1000;
    frames = (u32int*)kmalloc(INDEX_FROM_BIT(nframes));
    memset(frames, 0, INDEX_FROM_BIT(nframes));
    kernel_directory = (page_directory_t*) kmalloc_aligned(sizeof(page_directory_t));
    memset(kernel_directory, 0, sizeof(page_directory_t));
    kernel_directory->physicalAddress = (u32int) kernel_directory;
    current_directory = kernel_directory;
    int i = 0; //assign page fot heap region first
    for (i = HEAP_START; i < HEAP_START + HEAP_INITIAL_SIZE; i += 0x1000)
        get_page(i, 1, kernel_directory);
    i = 0; //then assign page form 0 to placement_address
    while (i < placement_address + 0x1000)
    {
        alloc_frame(get_page(i, 1, kernel_directory), 0, 0); //kernel code is readable but not writable from users space
        i += 0x1000;
    }
    for(i = HEAP_START; i < HEAP_START + HEAP_INITIAL_SIZE; i += 0x1000)
        alloc_frame(get_page(i, 1, kernel_directory), 0, 0);
    register_interrupt_handler(14, &page_fault_callback); //register the interrupt handler
    switch_page_directory(kernel_directory); //Enable Paging
    kheap = create_heap(HEAP_START, HEAP_START + HEAP_INITIAL_SIZE, 0xCFFFF000, 0 , 0);
    if (kheap){
        monitor_write("-Kernel Heap Initialization Success\n");
        monitor_write("--Kernel Heap Starts At: ");
        monitor_write_hex(kheap->start_address);
        monitor_put('\n');
        monitor_write("--Kernel Heap Ends At: ");
        monitor_write_hex(kheap->end_address);
        monitor_put('\n');
        monitor_write("--Kernel Heap Max Address At: ");
        monitor_write_hex(kheap->max_address);
        monitor_put('\n');
        monitor_write("--Kernel Heap Block Size: ");
        monitor_write_dec(kheap->index.size);
        monitor_put('\n');
    } else {
        monitor_write("-Kernel Heap Initialization Failed\n");
    }
    monitor_write("--Current Page Directory Is Located At: ");
    monitor_write_hex(current_directory->physicalAddress);
    monitor_put('\n');
    
    // for (int i = 0; i < 1024; i++)
    // {
    //     monitor_write("---Page table ");
    //     monitor_write_dec(i);
    //     monitor_put('\n');
    //     page_table_t * table = current_directory->tables[i];
    //     for (int j = 0; j < 1024; j++){
    //         monitor_write("---Page ");
    //         monitor_write_dec(j);
    //         if(table->pages[j].present){
    //             monitor_write(" Page Is Present Physcially At ");
    //             monitor_write_hex(table->pages[j].frame);
    //         } else {
    //             monitor_write(" Page Is Not Present Physcially");
    //         }
    //         monitor_put('\n');
    //     }
    // }
    

}

void switch_page_directory(page_directory_t *dir){
    current_directory = dir;
    asm volatile("mov %0, %%cr3"::"r"(&dir->tablesPhysical));
    u32int cr0;
    asm volatile("mov %%cr0, %0":"=r"(cr0));
    cr0 |= 0x80000000;
    asm volatile("mov %0, %%cr0"::"r"(cr0));
}

page_t *get_page(u32int adddress, int make, page_directory_t *dir){
    adddress /= 0x1000; //Turns address into index
    u32int table_index = adddress / 1024;
    if(dir->tables[table_index]){
        return &dir->tables[table_index]->pages[adddress%1024];
    }
    else if (make){
        u32int phy_addr;
        dir->tables[table_index] = (page_table_t*) kmalloc_aligned_physical(sizeof(page_table_t), &phy_addr);
        memset(dir->tables[table_index], 0, 0x1000);
        dir->tablesPhysical[table_index] = phy_addr | 0x7; //Present, RW, US
        return &dir->tables[table_index]->pages[adddress%1024];
    } else{
        return 0;
    }
}

void page_fault_callback(registers_t regs){
    u32int faulting_address;
    asm volatile("mov %%cr2, %0" : "=r" (faulting_address));
    int (present) = !(regs.err_code & 0x01);
    int (rw) = (regs.err_code & 0x02);
    int (us) = (regs.err_code & 0x04);
    int (reserved) = (regs.err_code & 0x10);
    monitor_write("page fault! (");
    if(present){monitor_write("present ");}
    if(rw){monitor_write("read-only ");}
    if(us){monitor_write("usermode ");}
    if(reserved){monitor_write("reserved ");}
    monitor_write(") at ");
    monitor_write_hex(faulting_address);
    monitor_write("\n");
}
