CAQTDM_PLUGINNAME = graphics
include(./plugins.pri)

SOURCES	+= qtcontrols_graphics_plugin.cpp
HEADERS	+= qtcontrols_graphics_plugin.h designerPluginTexts.h
RESOURCES += qtcontrolsplugin.qrc
TARGET = qtcontrols_graphics_plugin

android {
   INCLUDEPATH += $(ANDROIDFUNCTIONSINCLUDE)
}
