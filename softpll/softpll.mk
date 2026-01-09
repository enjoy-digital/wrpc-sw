obj-softpll = \
	softpll/spll_common.o \
	softpll/spll_external.o \
	softpll/spll_helper.o \
	softpll/spll_main.o \
	softpll/spll_ptracker.o \
	softpll/softpll_ng.o

# softpll is used for nodes and for switch
# But not for host process.
obj-$(CONFIG_EMBEDDED_NODE) += $(obj-softpll)
obj-$(CONFIG_TARGET_WR_SWITCH) += $(obj-softpll)
