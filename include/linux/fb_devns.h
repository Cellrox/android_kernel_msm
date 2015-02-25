#ifndef _LINUX_FB_DEVNS_H
#define _LINUX_FB_DEVNS_H

#ifdef __KERNEL__

#ifdef CONFIG_FB_DEV_NS

extern struct fb_info *get_fb_info_ns(struct fb_info *fb_info);
extern void put_fb_info_ns(struct fb_info *fb_info);

extern struct fb_info *fb_virt_to_info(struct fb_info *fb_virt);
extern struct fb_info *fb_virt_to_info_ns(struct fb_info *fb_virt);

extern int track_fb_inode(struct fb_info *fb_info, struct inode *inode);
extern void untrack_fb_inode(struct fb_info *fb_info, struct inode *inode);

#else

#define fb_virt_to_info(fb_info)  fb_info
#define fb_virt_to_info_ns(fb_info)  fb_info

#endif

#endif /* __KERNEL__ */

#endif /* _LINUX_FB_DEVNS_H */
