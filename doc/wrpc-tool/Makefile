TOOL_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
PDF := $(TOOL_DIR)/build/wrpc_tool_doc.pdf
DOC := $(TOOL_DIR)/wrpc_tool.tex

.PHONY: all clean

all: $(PDF)

$(PDF): $(DOC)
	@mkdir -p $(TOOL_DIR)/build
	@latexmk -pdf -interaction=nonstopmode -halt-on-error \
		-outdir=$(TOOL_DIR)/build $(DOC) >/tmp/wrpc_tool_latexmk.log 2>&1 || { \
		cat /tmp/wrpc_tool_latexmk.log; exit 1; }
	@echo "Built $@"

clean:
	rm -rf $(TOOL_DIR)/build