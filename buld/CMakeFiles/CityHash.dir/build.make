# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.18

# Delete rule output on recipe failure.
.DELETE_ON_ERROR:


#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:


# Disable VCS-based implicit rules.
% : %,v


# Disable VCS-based implicit rules.
% : RCS/%


# Disable VCS-based implicit rules.
% : RCS/%,v


# Disable VCS-based implicit rules.
% : SCCS/s.%


# Disable VCS-based implicit rules.
% : s.%


.SUFFIXES: .hpux_make_needs_suffix_list


# Command-line flag to silence nested $(MAKE).
$(VERBOSE)MAKESILENT = -s

#Suppress display of executed commands.
$(VERBOSE).SILENT:

# A target that is always out of date.
cmake_force:

.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /usr/bin/cmake

# The command to remove a file.
RM = /usr/bin/cmake -E rm -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /home/bencz/programming/yet-another-smalltalk-vm

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /home/bencz/programming/yet-another-smalltalk-vm/buld

# Include any dependencies generated for this target.
include CMakeFiles/CityHash.dir/depend.make

# Include the progress variables for this target.
include CMakeFiles/CityHash.dir/progress.make

# Include the compile flags for this target's objects.
include CMakeFiles/CityHash.dir/flags.make

CMakeFiles/CityHash.dir/cityhash/city.c.o: CMakeFiles/CityHash.dir/flags.make
CMakeFiles/CityHash.dir/cityhash/city.c.o: ../cityhash/city.c
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/home/bencz/programming/yet-another-smalltalk-vm/buld/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building C object CMakeFiles/CityHash.dir/cityhash/city.c.o"
	/usr/bin/cc $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -o CMakeFiles/CityHash.dir/cityhash/city.c.o -c /home/bencz/programming/yet-another-smalltalk-vm/cityhash/city.c

CMakeFiles/CityHash.dir/cityhash/city.c.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing C source to CMakeFiles/CityHash.dir/cityhash/city.c.i"
	/usr/bin/cc $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -E /home/bencz/programming/yet-another-smalltalk-vm/cityhash/city.c > CMakeFiles/CityHash.dir/cityhash/city.c.i

CMakeFiles/CityHash.dir/cityhash/city.c.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling C source to assembly CMakeFiles/CityHash.dir/cityhash/city.c.s"
	/usr/bin/cc $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -S /home/bencz/programming/yet-another-smalltalk-vm/cityhash/city.c -o CMakeFiles/CityHash.dir/cityhash/city.c.s

# Object files for target CityHash
CityHash_OBJECTS = \
"CMakeFiles/CityHash.dir/cityhash/city.c.o"

# External object files for target CityHash
CityHash_EXTERNAL_OBJECTS =

libCityHash.so: CMakeFiles/CityHash.dir/cityhash/city.c.o
libCityHash.so: CMakeFiles/CityHash.dir/build.make
libCityHash.so: CMakeFiles/CityHash.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --bold --progress-dir=/home/bencz/programming/yet-another-smalltalk-vm/buld/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Linking C shared library libCityHash.so"
	$(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/CityHash.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
CMakeFiles/CityHash.dir/build: libCityHash.so

.PHONY : CMakeFiles/CityHash.dir/build

CMakeFiles/CityHash.dir/clean:
	$(CMAKE_COMMAND) -P CMakeFiles/CityHash.dir/cmake_clean.cmake
.PHONY : CMakeFiles/CityHash.dir/clean

CMakeFiles/CityHash.dir/depend:
	cd /home/bencz/programming/yet-another-smalltalk-vm/buld && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /home/bencz/programming/yet-another-smalltalk-vm /home/bencz/programming/yet-another-smalltalk-vm /home/bencz/programming/yet-another-smalltalk-vm/buld /home/bencz/programming/yet-another-smalltalk-vm/buld /home/bencz/programming/yet-another-smalltalk-vm/buld/CMakeFiles/CityHash.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : CMakeFiles/CityHash.dir/depend
