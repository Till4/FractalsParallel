#include <stdio.h>
#include <stdlib.h>
#include <omp.h>
#include <string.h>

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

void compute_and_output(double zoom, double centerX, double centerY) {
    double scale = 4.0 / (WIDTH * zoom);
    unsigned char *rgb_buffer = malloc(3 * WIDTH * HEIGHT);
    if (!rgb_buffer) {
        fprintf(stderr, "Error allocating memory\n");
        exit(1);
    }

#pragma omp parallel for collapse(2)
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            double cx = centerX + (x - WIDTH / 2) * scale;
            double cy = centerY + (y - HEIGHT / 2) * scale;
            int iter = mandelbrot(cx, cy);

            int index = (y * WIDTH + x) * 3;
            if (iter == MAX_ITER) {
                rgb_buffer[index + 0] = 0;
                rgb_buffer[index + 1] = 0;
                rgb_buffer[index + 2] = 0;
            } else {
                float hue = (float)iter / MAX_ITER;
                rgb_buffer[index + 0] = (unsigned char)(hue * 255);
                rgb_buffer[index + 1] = (unsigned char)((1 - hue) * 255);
                rgb_buffer[index + 2] = (unsigned char)(hue * 128);
            }
        }
    }

    fwrite(rgb_buffer, 1, 3 * WIDTH * HEIGHT, stdout);
    fflush(stdout);
    free(rgb_buffer);
}

int main() {
    char line[256];
    double zoom, centerX, centerY;

    while (fgets(line, sizeof(line), stdin)) {
        if (sscanf(line, "%lf %lf %lf", &zoom, &centerX, &centerY) != 3) {
            fprintf(stderr, "Invalid input: expected 3 floats (zoom centerX centerY)\n");
            continue;
        }
        compute_and_output(zoom, centerX, centerY);
    }

    return 0;
}