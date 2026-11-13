# Project Nayuki's QR Code generator (MIT), vendored unmodified: see README.md.
add_library(qrcodegen STATIC
        qrcodegen.cpp
        qrcodegen.hpp
)
set_target_properties(qrcodegen PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
