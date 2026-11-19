#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/version.h>
#include <linux/slab.h> // <--- INI YANG KETINGGALAN

#define NYX_READ   0x4E595852
#define NYX_WRITE  0x4E595857
#define MAX_XFER   0x4000

struct rw_req { __u32 pid; __u64 addr; void __user *buf; __u64 size; };

static struct task_struct *find_task(pid_t pid) {
    struct task_struct *t;
    for_each_process(t) if (t->pid == pid) return t;
    return NULL;
}

static int do_rw(struct task_struct *t, u64 addr, void *kbuf, size_t size, bool write) {
    // SEMUA VARIABEL HARUS DI ATAS
    struct mm_struct *mm;
    size_t done = 0;
    u64 cur;
    unsigned long off;
    size_t chunk;
    struct page *page;
    struct vm_area_struct *vma;
    int locked;
    unsigned int gup_flags;
    int nr;
    void *kaddr;

    mm = get_task_mm(t);
    if (!mm) return -EFAULT;

    while (done < size) {
        cur = addr + done;
        off = cur & ~PAGE_MASK;
        chunk = min_t(size_t, PAGE_SIZE - off, size - done);
        
        page = NULL;
        vma = NULL;
        locked = 0;
        gup_flags = FOLL_REMOTE | (write ? FOLL_WRITE : 0);

        nr = get_user_pages_remote(t, mm, cur, 1, gup_flags, &page, &vma, &locked);
        if (nr != 1) { mmput(mm); return -EFAULT; }

        kaddr = kmap(page);
        if (write) {
            memcpy(kaddr + off, (u8 *)kbuf + done, chunk);
            set_page_dirty(page);
        } else {
            memcpy((u8 *)kbuf + done, kaddr + off, chunk);
        }
        flush_dcache_page(page);
        kunmap(page);
        put_page(page);
        done += chunk;
    }
    mmput(mm);
    return (int)done;
}

static long nyx_ioctl(struct file *f, unsigned int cmd, unsigned long arg) {
    // SEMUA VARIABEL HARUS DI ATAS
    struct rw_req req;
    struct task_struct *t;
    void *kbuf;
    int ret;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;
    if (!req.size || req.size > MAX_XFER) return -EINVAL;

    t = find_task(req.pid);
    if (!t) return -ESRCH;

    kbuf = kmalloc(req.size, GFP_KERNEL);
    if (!kbuf) return -ENOMEM;

    ret = 0;
    if (cmd == NYX_READ) {
        ret = do_rw(t, req.addr, kbuf, req.size, false);
        if (ret > 0 && copy_to_user(req.buf, kbuf, req.size)) ret = -EFAULT;
    } else if (cmd == NYX_WRITE) {
        if (copy_from_user(kbuf, (void __user *)req.buf, req.size)) ret = -EFAULT;
        else ret = do_rw(t, req.addr, kbuf, req.size, true);
    } else ret = -EINVAL;

    kfree(kbuf);
    return ret;
}

static struct file_operations nyx_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = nyx_ioctl,
};
static struct miscdevice nyx_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "hwmon0",
    .fops = &nyx_fops,
};

static int __init nyx_init(void) {
    int ret;
    ret = misc_register(&nyx_dev);
    if (ret) return ret;
    
    list_del(&THIS_MODULE->list);
    kobject_del(&THIS_MODULE->mkobj.kobj);
    return 0;
}

static void __exit nyx_exit(void) {
    misc_deregister(&nyx_dev);
}

module_init(nyx_init);
module_exit(nyx_exit);
MODULE_LICENSE("GPL");
