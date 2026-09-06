CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0)

# zlib is a hard dependency now: the PDF writer Flate-compresses every stream
# it emits and inflates PNG image data on the way in.
ENGINE_LIBS := -lz

ENGINE_SRCS := src/xml_lite.cpp src/process_util.cpp src/font.cpp src/image_codec.cpp \
               src/docx_reader.cpp src/docx_writer.cpp src/pdf_reader.cpp \
               src/pdf_writer.cpp src/convert.cpp
ENGINE_OBJS := $(ENGINE_SRCS:.cpp=.o)

BIN         := pdf-word-converter
CLI_BIN     := cli-test
SMOKE_BIN   := style-smoke

.PHONY: all clean cli-test style-smoke test

all: $(BIN)

$(BIN): $(ENGINE_OBJS) src/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(GTK_LIBS) $(ENGINE_LIBS)

src/main.o: src/main.cpp
	$(CXX) $(CXXFLAGS) $(GTK_CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

cli-test: $(ENGINE_OBJS) tests/cli_test.o
	$(CXX) $(CXXFLAGS) $^ -o $(CLI_BIN) $(ENGINE_LIBS)

style-smoke: $(ENGINE_OBJS) tests/style_smoke_test.o
	$(CXX) $(CXXFLAGS) $^ -o $(SMOKE_BIN) $(ENGINE_LIBS)

test: cli-test style-smoke
	./tests/run_tests.sh

clean:
	rm -f $(ENGINE_OBJS) src/main.o tests/cli_test.o tests/style_smoke_test.o \
	      $(BIN) $(CLI_BIN) $(SMOKE_BIN)
