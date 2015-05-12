# Manpage rules, for Libreswan.
#
# Copyright (C) 2015 Andrew Cagney <cagney@gnu.org>
# 
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2 of the License, or (at your
# option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
# 
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.

# NOTE: libreswan includes custom makefile configuration first, hence
# need a weak assign

XMLTO ?= xmlto

# $(MANDIR$(suffix $(MANPAGE))) will expand one of the below, roughly:
# 3 is libraries; 8 is for system programs; and 5 is for file formats.
MANDIR.3 ?= $(MANTREE)/man3
MANDIR.5 ?= $(MANTREE)/man5
MANDIR.8 ?= $(MANTREE)/man8

# PROGRAM=pluto -> MANPAGES+=ipsec_pluto.8
ifeq ($(origin MANPROGPREFIX),undefined)
MANPROGPREFIX ?= ipsec_
endif

# If PROGRAM is empty, PROGRAM_MANPAGE also ends up empty.
PROGRAM_MANPAGE = $(addprefix $(MANPROGPREFIX), $(addsuffix .8, $(PROGRAM)))
MANPAGES += $(PROGRAM_MANPAGE)

local-manpages: $(MANPAGES)

# Generate a list of <refname/> entries, including the section number,
# from the original xml source.

refnames = $(foreach manpage, $(1), \
		$(addsuffix $(suffix $(manpage)), \
			$(shell $(abs_top_srcdir)/packaging/utils/refnames.sh $(manpage).xml)))

ifeq ($(srcdir),.)
install-local-manpages: local-manpages
	@set -eu ; $(foreach refname, $(call refnames,$(MANPAGES)), \
		src=$(builddir)/$(refname) ; \
		destdir=$(MANDIR$(suffix $(refname))) ; \
		echo $$src '->' $$destdir ; \
		mkdir -p $$destdir ; \
		$(INSTALL) $(INSTMANFLAGS) $$src $$destdir ; \
	)
else
# install manpage target is designed to work in $(srcdir)
install-local-manpages:
	$(MAKE) -C $(srcdir) $@
endif

list-local-manpages:
	@set -eu ; $(foreach refname, $(call refnames,$(MANPAGES)), \
		echo $(MANDIR$(suffix $(refname)))/$(refname) ; \
	)

clean-local-manpages:
	rm -f $(builddir)/*.[1-8]

# Always write the output to $(builddir).
#
# Note: XMLTO seems to fail even when it succeeds so ignore its status
# and check for the expected output.
#
# Note: When the .xml file is generated, $@ will already include the
# path to $(builddir); so strip that off.

%: %.xml
	: VPATH=$(VPATH)
	@mkdir -p $(builddir)
	-$(XMLTO) -o $(builddir) man $<
	test -r $(builddir)/$(notdir $@)
