#
# Makefile for the SGI specific kernel interface routines
# under Linux.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#
# Note 2! The CFLAGS definitions are now in the main makefile...

USE_STANDARD_AS_RULE := true

O_TARGET := ip32-kern.a

all: ip32-kern.a ip32-irq-glue.o

obj-y	+= ip32-berr.o ip32-rtc.o ip32-setup.o ip32-irq.o ip32-irq-glue.o \
	   ip32-timer.o crime.o ip32-reset.o

obj-$(CONFIG_PCI)   += ip32-pci.o

include $(TOPDIR)/Rules.make
