OPTS = -O3 -std=c++11
MPICC = mpicxx $(OPTS)
NVCC = nvcc $(OPTS)

TARGET = jacobi3d
OBJS = $(TARGET).o $(TARGET)CUDA.o

all: $(OBJS)
	$(MPICC) -o $(TARGET) $^ -lcuda -lcudart -L$(CUDA_DIR)/lib64

$(TARGET).o: $(TARGET).C $(TARGET).h
	$(MPICC) -o $@ -c $<

$(TARGET)CUDA.o: $(TARGET).cu $(TARGET).h
	$(NVCC) -o $@ -c $<

clean:
	rm -f *.o $(TARGET)
