.globl _start

// Branch to kernel (m1n1) in EL3
// struct monitor_boot_args {
//    u64 version;
//    u64 virt_base;
//    u64 phys_base;
//    u64 mem_size;
//    struct boot_args *kern_args;
//    void (*kern_entry)(struct boot_args*);
//    u64 kern_phys_base;
//    u64 kern_phys_slide;
//    u64 kern_virt_slide;
//    u64 kern_header_off;
// };

.type _start, @function
_start:
    ldp x0, x30, [x0, #0x20]
    ret
