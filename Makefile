CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0)

ENGINE_SRCS := src/xml_lite.cpp src/process_util.cpp src/docx_reader.cpp \
               src/docx_writer.cpp src/pdf_reader.cpp src/pdf_writer.cpp src/convert.cpp
ENGINE_OBJS := $(ENGINE_SRCS:.cpp=.o)

BIN         := pdf-word-converter
CLI_BIN     := cli-test

.PHONY: all clean cli-test

all: $(BIN)

$(BIN): $(ENGINE_OBJS) src/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(GTK_LIBS)

src/main.o: src/main.cpp
	$(CXX) $(CXXFLAGS) $(GTK_CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

cli-test: $(ENGINE_OBJS) tests/cli_test.o
	$(CXX) $(CXXFLAGS) $^ -o $(CLI_BIN)

clean:
	rm -f $(ENGINE_OBJS) src/main.o tests/cli_test.o $(BIN) $(CLI_BIN)
