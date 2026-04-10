TARGET = nshcpuset.exe
SRC = nshcpuset.cpp

all:
	cl /nologo /O2 /W3 /Fe:$(TARGET) $(SRC)

clean:
	del /Q $(TARGET) 2>NUL