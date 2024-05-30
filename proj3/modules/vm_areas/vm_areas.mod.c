#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/export-internal.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif


static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x92997ed8, "_printk" },
	{ 0xe9792222, "misc_register" },
	{ 0x72586034, "misc_deregister" },
	{ 0x9688de8b, "memstart_addr" },
	{ 0x20cef023, "find_vpid" },
	{ 0xea1fae85, "pid_task" },
	{ 0xc6f861b7, "mas_find" },
	{ 0x8da6585d, "__stack_chk_fail" },
	{ 0x7682ba4e, "__copy_overflow" },
	{ 0x12a4e128, "__arch_copy_from_user" },
	{ 0x98cf60b3, "strlen" },
	{ 0x77bc13a0, "strim" },
	{ 0x8c8569cb, "kstrtoint" },
	{ 0xad8809de, "module_layout" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "5A6B3EECEEB8DE64A345C8A");
