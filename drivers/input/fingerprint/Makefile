#
# Makefile for the fingerprint drivers.
#

# Each configuration option enables a list of files.

obj-$(CONFIG_FINGERPRINT_GOODIX_TA)	+= goodix_ta/
obj-$(CONFIG_FINGERPRINT_FPC1268_TEE)	+= fpc1268_tee/

#fingerprint_detect should before fpc1022
obj-$(CONFIG_FINGERPRINT_DETECT)    += fingerprint_detect/
obj-$(CONFIG_FINGERPRINT_FPC)	    += fpc/
obj-$(CONFIG_FINGERPRINT_GOODIX)    += goodix/
