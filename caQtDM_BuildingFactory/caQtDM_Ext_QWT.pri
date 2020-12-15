Define_Build_qwt {

    INCLUDEPATH += $$(QWTINCLUDE)
        unix:!macx:!ios:!android {
                LIBS += -L$$(QWTLIB) -Wl,-rpath,$(QWTLIB) -l$$(QWTLIBNAME)
  	}

        macx {
		LIBS += -F$$(QWTLIB) -framework $$(QWTLIBNAME)
  	}

	ios | android {
        }

	win32 {
	
    		win32-g++ {
      			INCLUDEPATH = $(QWTHOME)/src
                        LIBS += $$(QWTLIB)/lib$$(QWTLIBNAME).a
     		}
     		win32-msvc* || msvc{
		     DebugBuild {
			 message("QWT Debug configuration : win32")
			 LIBS += $$(QWTHOME)/lib/$$(QWTLIBNAME)d.lib
		     }
		    ReleaseBuild {
			message("QWT Release configuration : win32")
			LIBS += $$(QWTHOME)/lib/$$(QWTLIBNAME).lib
		    }
	    }
	}
}