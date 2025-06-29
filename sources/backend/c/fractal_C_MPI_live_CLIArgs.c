#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include <math.h>

#define DEFAULT_WIDTH 800
#define DEFAULT_HEIGHT 600
#define DEFAULT_MAX_ITER 200
#define CHUNK_SIZE 10  // Number of rows per dynamic task chunk

// Enable verbose debug output (set to 1 to enable)
#define VERBOSE 0

// Clamp a value between min and max
static inline double clamp(double v, double min, double max) {
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

// Map pixel coordinate to complex plane coordinate
static inline double map_pixel_to_complex(int pixel, int dimension, double center, double scale) {
    return center + (pixel - dimension / 2) * scale;
}

// Compute Mandelbrot iterations with smooth coloring for better gradients
int mandelbrot(double x0, double y0, int max_iter) {
    double x = 0.0, y = 0.0;
    int iter = 0;
    while (x*x + y*y <= 4.0 && iter < max_iter) {
        double xtemp = x*x - y*y + x0;
        y = 2*x*y + y0;
        x = xtemp;
        iter++;
    }

    if (iter == max_iter) {
        return max_iter;
    }

    // Smooth iteration count for better coloring
    double log_zn = log(x*x + y*y) / 2.0;
    double nu = log(log_zn / log(2)) / log(2);
    return iter + 1 - nu;
}

// Converts a smooth iteration count to RGB color using HSV->RGB approximation
void iteration_to_color(double iter, int max_iter, unsigned char* rgb) {
    if (iter >= max_iter) {
        // Inside Mandelbrot set -> black
        rgb[0] = rgb[1] = rgb[2] = 0;
        return;
    }

    // Normalize iteration count to [0,1]
    double t = iter / max_iter;

    // HSV-like coloring: hue varies, saturation=1, value=1
    double hue = 360.0 * t;
    double c = 1.0;
    double x = c * (1 - fabs(fmod(hue / 60.0, 2) - 1));
    double m = 0;

    double r, g, b;
    if (hue < 60)      { r = c; g = x; b = 0; }
    else if (hue < 120) { r = x; g = c; b = 0; }
    else if (hue < 180) { r = 0; g = c; b = x; }
    else if (hue < 240) { r = 0; g = x; b = c; }
    else if (hue < 300) { r = x; g = 0; b = c; }
    else                { r = c; g = 0; b = x; }

    rgb[0] = (unsigned char)((r + m) * 255);
    rgb[1] = (unsigned char)((g + m) * 255);
    rgb[2] = (unsigned char)((b + m) * 255);
}

// Compute a block of rows from startY to endY (exclusive)
void compute_rows(double zoom, double centerX, double centerY,
                  int startY, int endY, int width, int height,
                  int max_iter, unsigned char* buffer) {

    double scale = 4.0 / (width * zoom);

    for (int y = startY; y < endY; y++) {
        for (int x = 0; x < width; x++) {
            double cx = map_pixel_to_complex(x, width, centerX, scale);
            double cy = map_pixel_to_complex(y, height, centerY, scale);
            double iter = (double)mandelbrot(cx, cy, max_iter);

            int localY = y - startY;
            int index = (localY * width + x) * 3;

            iteration_to_color(iter, max_iter, &buffer[index]);
        }
    }
}

// Prints debug messages if VERBOSE is enabled and rank is zero or specified rank
void debug_log(int rank, const char* fmt, ...) {
    if (!VERBOSE) return;
    if (rank == 0) {
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
    }
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    // Parse optional command line arguments for width, height, max_iter
    int width = DEFAULT_WIDTH;
    int height = DEFAULT_HEIGHT;
    int max_iter = DEFAULT_MAX_ITER;

    if (argc >= 2) width = atoi(argv[1]);
    if (argc >= 3) height = atoi(argv[2]);
    if (argc >= 4) max_iter = atoi(argv[3]);

    // Check sanity of input
    if (width <= 0) width = DEFAULT_WIDTH;
    if (height <= 0) height = DEFAULT_HEIGHT;
    if (max_iter <= 0) max_iter = DEFAULT_MAX_ITER;

    if (rank == 0) {
        fprintf(stderr, "Running Mandelbrot with %d x %d pixels, max_iter=%d, processes=%d\n",
                width, height, max_iter, size);
        fprintf(stderr, "Input format: zoom centerX centerY (one set per line)\n");
    }

    double zoom, centerX, centerY;

    // Buffers for gathering results (only allocated on rank 0)
    unsigned char* fullBuffer = NULL;
    int* recvCounts = NULL;
    int* displs = NULL;

    if (rank == 0) {
        fullBuffer = malloc(3 * width * height);
        if (!fullBuffer) {
            fprintf(stderr, "Rank 0: Failed to allocate fullBuffer\n");
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }

        recvCounts = malloc(sizeof(int) * size);
        displs = malloc(sizeof(int) * size);
        if (!recvCounts || !displs) {
            fprintf(stderr, "Rank 0: Failed to allocate gather metadata\n");
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }
    }

    // Main loop: rank 0 reads input lines, broadcasts parameters, all compute image
    while (1) {
        if (rank == 0) {
            char line[256];
            if (!fgets(line, sizeof(line), stdin)) {
                // No more input, send termination signal (zoom <= 0)
                zoom = -1.0;
                centerX = centerY = 0.0;
            } else {
                if (sscanf(line, "%lf %lf %lf", &zoom, &centerX, &centerY) != 3) {
                    fprintf(stderr, "Invalid input line, expected: zoom centerX centerY\n");
                    zoom = -1.0; // Terminate on error input
                }
            }
        }

        // Broadcast zoom param; if zoom <= 0, terminate
        MPI_Bcast(&zoom, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);
        if (zoom <= 0) break;

        // Broadcast centerX and centerY to all processes
        MPI_Bcast(&centerX, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);
        MPI_Bcast(&centerY, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);

        if (VERBOSE && rank == 0) {
            fprintf(stderr, "Computing image: zoom=%.6f center=(%.6f, %.6f)\n", zoom, centerX, centerY);
        }

        // Dynamic load balancing via master-worker pattern:
        // Master manages chunks of rows to assign to workers on request.

        if (rank == 0) {
            int nextRow = 0;
            int activeWorkers = size - 1;
            MPI_Status status;

            // Store partial results from workers
            while (activeWorkers > 0) {
                int workerRank, resultStartY;
                // Probe for incoming message from any worker
                MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
                workerRank = status.MPI_SOURCE;

                if (status.MPI_TAG == 1) {
                    // Worker requests new chunk (send startY)
                    int dummy;
                    MPI_Recv(&dummy, 0, MPI_INT, workerRank, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                    if (nextRow >= height) {
                        // No more rows to assign, send termination chunk
                        int terminator = -1;
                        MPI_Send(&terminator, 1, MPI_INT, workerRank, 2, MPI_COMM_WORLD);
                    } else {
                        // Assign chunk
                        int chunkStart = nextRow;
                        int chunkEnd = nextRow + CHUNK_SIZE;
                        if (chunkEnd > height) chunkEnd = height;

                        int chunkSize = chunkEnd - chunkStart;
                        MPI_Send(&chunkStart, 1, MPI_INT, workerRank, 2, MPI_COMM_WORLD);
                        MPI_Send(&chunkSize, 1, MPI_INT, workerRank, 2, MPI_COMM_WORLD);

                        nextRow = chunkEnd;
                    }
                } else if (status.MPI_TAG == 3) {
                    // Worker sends computed data chunk
                    // First receive startY and chunkSize
                    MPI_Recv(&resultStartY, 1, MPI_INT, workerRank, 3, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    int resultSize;
                    MPI_Recv(&resultSize, 1, MPI_INT, workerRank, 3, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                    // Receive actual pixel data
                    MPI_Recv(fullBuffer + 3 * width * resultStartY,
                             3 * width * resultSize, MPI_UNSIGNED_CHAR,
                             workerRank, 3, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                    if (VERBOSE) {
                        fprintf(stderr, "Received chunk %d..%d from rank %d\n",
                                resultStartY, resultStartY + resultSize - 1, workerRank);
                    }
                } else if (status.MPI_TAG == 4) {
                    // Worker signals done and disconnects
                    MPI_Recv(NULL, 0, MPI_INT, workerRank, 4, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    activeWorkers--;
                }
            }

            // Output image in PPM format to stdout
            printf("P6\n%d %d\n255\n", width, height);
            fwrite(fullBuffer, 1, 3 * width * height, stdout);
            fflush(stdout);

        } else {
            // Worker processes request chunks, compute, and send results back

            while (1) {
                // Request chunk
                MPI_Send(NULL, 0, MPI_INT, 0, 1, MPI_COMM_WORLD);

                int startY, chunkSize;
                MPI_Recv(&startY, 1, MPI_INT, 0, 2, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                if (startY == -1) {
                    // No more work, send done signal and break
                    MPI_Send(NULL, 0, MPI_INT, 0, 4, MPI_COMM_WORLD);
                    break;
                }

                MPI_Recv(&chunkSize, 1, MPI_INT, 0, 2, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                if (VERBOSE) {
                    fprintf(stderr, "Rank %d computing rows %d..%d\n", rank, startY, startY + chunkSize - 1);
                }

                unsigned char* localBuffer = malloc(3 * width * chunkSize);
                if (!localBuffer) {
                    fprintf(stderr, "Rank %d: malloc failed\n", rank);
                    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
                }

                compute_rows(zoom, centerX, centerY, startY, startY + chunkSize,
                             width, height, max_iter, localBuffer);

                // Send computed chunk to master
                MPI_Send(&startY, 1, MPI_INT, 0, 3, MPI_COMM_WORLD);
                MPI_Send(&chunkSize, 1, MPI_INT, 0, 3, MPI_COMM_WORLD);
                MPI_Send(localBuffer, 3 * width * chunkSize, MPI_UNSIGNED_CHAR, 0, 3, MPI_COMM_WORLD);

                free(localBuffer);
            }
        }
    }

    // Cleanup
    if (rank == 0) {
        free(fullBuffer);
        free(recvCounts);
        free(displs);
    }

    MPI_Finalize();
    return 0;
}