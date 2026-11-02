	.global _cpu_init_hook
	.type _cpu_init_hook, %function
_cpu_init_hook:
	mov x0, #(3 << 20)
	msr cpacr_el1, x0
	isb
	ret
