# Note: plugindir is set in configure

##############################################################################
# TODO: change libgstplugin.la to something else, e.g. libmysomething.la     #
##############################################################################
plugin_LTLIBRARIES = libgstsrt.la

##############################################################################
# TODO: for the next set of variables, name the prefix if you named the .la, #
#  e.g. libmysomething.la => libmysomething_la_SOURCES                       #
#                            libmysomething_la_CFLAGS                        #
#                            libmysomething_la_LIBADD                        #
#                            libmysomething_la_LDFLAGS                       #
##############################################################################

## Plugin 1

# sources used to compile this plug-in
libgstsrt_la_SOURCES = \
	gstsrt.c \
	gstsrtbasesrc.c \
	gstsrtclientsrc.c \
	gstsrtserversrc.c \
	gstsrtbasesink.c \
	gstsrtclientsink.c \
	gstsrtserversink.c \
	gstsrtmeta.c \
	$(NULL)

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstsrt_la_CFLAGS = \
	$(GST_PLUGINS_BASE_CFLAGS) \
	$(GST_CFLAGS) \
	$(GIO_CFLAGS) \
	$(SRT_CFLAGS) \
	$(NULL)
libgstsrt_la_LIBADD = \
	$(GST_PLUGINS_BASE_LIBS) \
	$(GST_LIBS) \
	$(GIO_LIBS) \
	-lgio-2.0 \
	-lsrt \
	$(SRT_LIBS) \
	$(NULL)

#-../../srt/libsrt \
libgstplugin_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
#libgstplugin_la_LIBTOOLFLAGS = --tag=disable-static
CLEANFILES = $(BUILT_SOURCES)
# headers we need but don't want installed
#noinst_HEADERS = gstplugin.h
