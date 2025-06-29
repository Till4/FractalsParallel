#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>

#define WIDTH 800
#define HEIGHT 600
#define MAX_ITER 200

int mandelbrot(double x0, double y0) {
    double x = 0.0, y = 0.0;
    int iter = 0;
    while (x*x + y*y <= 4.0 && iter < MAX_ITER) {
        double xtemp = x*x - y*y + x0;
        y = 2*x*y + y0;
        x = xtemp;
        iter++;
    }
    return iter;
}

void compute_part(double zoom, double centerX, double centerY,
                  int startY, int endY, unsigned char* buffer) {

    double scale = 4.0 / (WIDTH * zoom);

    for (int y = startY; y < endY; y++) {
        for (int x = 0; x < WIDTH; x++) {
            double cx = centerX + (x - WIDTH / 2) * scale;
            double cy = centerY + (y - HEIGHT / 2) * scale;
            int iter = mandelbrot(cx, cy);

            int localY = y - startY;
            int index = (localY * WIDTH + x) * 3;

            if (iter == MAX_ITER) {
                buffer[index + 0] = 0;
                buffer[index + 1] = 0;
                buffer[index + 2] = 0;
            } else {
                float hue = (float)iter / MAX_ITER;
                buffer[index + 0] = (unsigned char)(hue * 255);
                buffer[index + 1] = (unsigned char)((1 - hue) * 255);
                buffer[index + 2] = (unsigned char)(hue * 128);
            }
        }
    }
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    char line[256];
    double zoom, centerX, centerY;

    while (fgets(line, sizeof(line), stdin)) {
        if (sscanf(line, "%lf %lf %lf", &zoom, &centerX, &centerY) != 3) {
            if (rank == 0)
                fprintf(stderr, "Invalid input: expected 3 floats (zoom centerX centerY)\n");
            continue;
        }

        int rowsPerRank = HEIGHT / size;
        int extra = HEIGHT % size;
        int startY = rank * rowsPerRank + (rank < extra ? rank : extra);
        int endY = startY + rowsPerRank + (rank < extra ? 1 : 0);
        int localHeight = endY - startY;

        unsigned char* localBuffer = malloc(3 * WIDTH * localHeight);
        compute_part(zoom, centerX, centerY, startY, endY, localBuffer);

        if (rank == 0) {
            unsigned char* fullBuffer = malloc(3 * WIDTH * HEIGHT);
            memcpy(fullBuffer, localBuffer, 3 * WIDTH * localHeight);

            for (int r = 1; r < size; r++) {
                int sY = r * rowsPerRank + (r < extra ? r : extra);
                int eY = sY + rowsPerRank + (r < extra ? 1 : 0);
                int recvHeight = eY - sY;
                MPI_Recv(fullBuffer + 3 * WIDTH * sY, 3 * WIDTH * recvHeight, MPI_UNSIGNED_CHAR, r, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            }

            fwrite(fullBuffer, 1, 3 * WIDTH * HEIGHT, stdout);
            fflush(stdout);
            free(fullBuffer);
        } else {
            MPI_Send(localBuffer, 3 * WIDTH * localHeight, MPI_UNSIGNED_CHAR, 0, 0, MPI_COMM_WORLD);
        }

        free(localBuffer);
    }

    MPI_Finalize();
    return 0;
}