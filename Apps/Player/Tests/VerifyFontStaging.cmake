if(NOT DEFINED PLAYER_CONTENT_DIR)
    message(FATAL_ERROR "PLAYER_CONTENT_DIR must identify the Player's staged content directory")
endif()

foreach(font IN ITEMS
    Roboto-Regular.ttf Roboto-Medium.ttf Roboto-Bold.ttf
    Roboto-Italic.ttf Roboto-MediumItalic.ttf Roboto-BoldItalic.ttf
    RobotoMono-Regular.ttf RobotoMono-Bold.ttf
    RobotoMono-Italic.ttf RobotoMono-BoldItalic.ttf
    LICENSE.txt)
    set(path "${PLAYER_CONTENT_DIR}/Assets/Fonts/${font}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Missing bundled UI font file: ${path}")
    endif()
    file(SIZE "${path}" size)
    if(size EQUAL 0)
        message(FATAL_ERROR "Empty bundled UI font file: ${path}")
    endif()
endforeach()
