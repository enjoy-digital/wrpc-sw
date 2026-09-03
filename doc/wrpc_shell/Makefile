SHELL_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
OUTPUT := $(SHELL_DIR)/build/wrpc_shell_commands.tex
PDF := $(SHELL_DIR)/build/wrpc_shell_commands_doc.pdf
DOC := $(SHELL_DIR)/wrpc_shell_commands_doc.tex
WRPC_ROOT := $(SHELL_DIR)/../..

SOURCES := $(WRPC_ROOT)/include/cmds.h \
	$(wildcard $(WRPC_ROOT)/shell/*.c $(WRPC_ROOT)/dev/*.c $(WRPC_ROOT)/lib/*.c)

.PHONY: all clean

all: $(OUTPUT) $(PDF)

$(OUTPUT): generate_shell_commands.py $(SOURCES)
	@python3 generate_shell_commands.py --output $@

$(PDF): $(OUTPUT) $(DOC)
	@latexmk -pdf -interaction=nonstopmode -halt-on-error \
		-outdir=$(SHELL_DIR)/build $(DOC) >/tmp/wrpc_shell_latexmk.log 2>&1 || { \
		cat /tmp/wrpc_shell_latexmk.log; exit 1; }
	@echo "Built $@"

clean:
	rm -rf $(SHELL_DIR)/build
