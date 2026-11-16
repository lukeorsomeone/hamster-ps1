# HAMSTER CAGE - PS1 build (based on the PSn00bSDK template)
cmake_minimum_required(VERSION 3.21)

project(
	hamster-cage
	LANGUAGES    C CXX ASM
	VERSION      0.1.0
	DESCRIPTION  "Hamster Cage - PS1 edition"
)

psn00bsdk_add_executable(hamster GPREL src/main.c src/game.c)

psn00bsdk_add_cd_image(
	iso      # Target name
	hamster  # Output file name (= hamster.bin + hamster.cue)
	iso.xml  # Path to config file
	DEPENDS hamster system.cnf
)
