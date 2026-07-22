CONFIG += c++2b

win*{
	QMAKE_CXXFLAGS += /Zi
	Release:QMAKE_CXXFLAGS += /GL
	Release:QMAKE_LFLAGS += /DEBUG:FULL /OPT:REF /OPT:ICF /TIME /LTCG /INCREMENTAL:NO
}

mac*{
	# Qt 6.10's qyieldcpu.h calls the ACLE intrinsic __yield() on Apple Silicon without including <arm_acle.h>,
	# so clang rejects it as an implicit declaration. Force-include the header so the intrinsic is declared.
	QMAKE_CXXFLAGS += -include arm_acle.h
}