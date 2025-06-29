#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <cuda_runtime.h>

__device__ int mandelbrot(double real, double imag, int max_iter)
{
    double z_real = 0.0, z_imag = 0.0;
    int iter = 0;
    while (z_real * z_real + z_imag * z_imag <= 4.0 && iter < max_iter)
    {
        double temp = z_real * z_real - z_imag * z_imag + real;
        z_imag = 2.0 * z_real * z_imag + imag;
        z_real = temp;
        iter++;
    }
    return iter;
}
__device__ void valueToRGB(int color, uint8_t &r, uint8_t &g, uint8_t &b)
{

    double h = (color % 360) / 360.0;
    double s = 0.8;
    double v = 1.0;

    if (color <= 0)
    {
        r = g = b = 0;
        return;
    }

    int i = (int)(h * 6);
    double f = h * 6 - i;
    double p = v * (1 - s);
    double q = v * (1 - f * s);
    double t = v * (1 - (1 - f) * s);

    switch (i % 6)
    {
    case 0:
        r = (uint8_t)(v * 255);
        g = (uint8_t)(t * 255);
        b = (uint8_t)(p * 255);
        break;
    case 1:
        r = (uint8_t)(q * 255);
        g = (uint8_t)(v * 255);
        b = (uint8_t)(p * 255);
        break;
    case 2:
        r = (uint8_t)(p * 255);
        g = (uint8_t)(v * 255);
        b = (uint8_t)(t * 255);
        break;
    case 3:
        r = (uint8_t)(p * 255);
        g = (uint8_t)(q * 255);
        b = (uint8_t)(v * 255);
        break;
    case 4:
        r = (uint8_t)(t * 255);
        g = (uint8_t)(p * 255);
        b = (uint8_t)(v * 255);
        break;
    case 5:
        r = (uint8_t)(v * 255);
        g = (uint8_t)(p * 255);
        b = (uint8_t)(q * 255);
        break;
    }
}

__global__ void render(uint8_t *image, double scale, double centerX, double centerY, int WIDTH, int HEIGHT)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= WIDTH || y >= HEIGHT)
        return;

    double real = (x - WIDTH / 2.0) * scale + centerX;
    double imag = (HEIGHT / 2.0 - y) * scale + centerY; // Korrigierte Y-Achse

    const double INITIAL_SCALE_AT_ZOOM_1 = 4.0 / WIDTH; // Dies ist ein konstanter Wert, der den Skalierungsfaktor bei Zoom 1 repräsentiert.

    int MAX_ITER = 256;
    if (scale > 0)
    {

        MAX_ITER += (int)(log(INITIAL_SCALE_AT_ZOOM_1 / scale) * 50.0);

        if (MAX_ITER < 100)
            MAX_ITER = 100;
        if (MAX_ITER > 8192)
            MAX_ITER = 8192;
    }

    int iter = mandelbrot(real, imag, MAX_ITER);
    int idx = 3 * (y * WIDTH + x);

    uint8_t color = 0;

    if (iter < MAX_ITER)
    {
        double normalized_iter = (double)iter / (double)MAX_ITER;
        color = (uint8_t)(sqrt(normalized_iter) * 255.0);
    }

    uint8_t r, g, b;
    valueToRGB(color, r, g, b);

    image[idx + 0] = r; // R
    image[idx + 1] = g; // G
    image[idx + 2] = b; // B
}

int main()
{
    fprintf(stderr, "CUDA Backend started\n");
    fflush(stderr);

    char line[256];
    
    // CUDA Events außerhalb der Schleife initialisieren, um wiederholte Erzeugung zu vermeiden
    cudaEvent_t start, stop;
    cudaEventCreate(&start);
    cudaEventCreate(&stop);

    // Initialisierung von d_image und h_image auf NULL
    // Dies hilft, potenzielle doppelte Freigaben zu vermeiden, wenn die Schleife vorzeitig beendet wird
    uint8_t *d_image = NULL;
    uint8_t *h_image = NULL;
    size_t currentImageSize = 0; // Speichert die aktuelle Größe des zugewiesenen Speichers

    while (fgets(line, sizeof(line), stdin))
    {
        int WIDTH;  // Breite des Bildes
        int HEIGHT; // Höhe des Bildes
        double zoom, centerX, centerY;
        
        if (sscanf(line, "%lf %lf %lf %d %d", &zoom, &centerX, &centerY, &WIDTH, &HEIGHT) != 5)
        {
            fprintf(stderr, "Invalid input: %s", line);
            fflush(stderr);
            continue;
        }
        
        size_t newImageSize = (size_t)WIDTH * HEIGHT * 3;

        // Speicher nur neu zuweisen, wenn die Größe sich ändert
        if (newImageSize != currentImageSize) {
            if (d_image) {
                cudaFree(d_image);
                d_image = NULL;
            }
            if (h_image) {
                free(h_image);
                h_image = NULL;
            }
            cudaMalloc(&d_image, newImageSize);
            h_image = (uint8_t *)malloc(newImageSize);
            
            if (h_image == NULL) {
                if (d_image) cudaFree(d_image);
                cudaEventDestroy(start);
                cudaEventDestroy(stop);
                return 1; 
            }
            if (cudaGetLastError() != cudaSuccess) {
                if (h_image) free(h_image);
                cudaEventDestroy(start);
                cudaEventDestroy(stop);
                return 1;
            }
            currentImageSize = newImageSize;
        }

        int blockSize = 16;

        dim3 block(blockSize, blockSize);
        dim3 grid((WIDTH + block.x - 1) / block.x, (HEIGHT + block.y - 1) / block.y);

        fprintf(stderr, "Received: zoom=%.2f, centerX=%.2f, centerY=%.2f, WIDTH=%d, HEIGHT=%d\n", zoom, centerX, centerY, WIDTH, HEIGHT);
        fflush(stderr);

        double scale = 4.0 / (WIDTH * zoom);

        // Timing START
        cudaEventRecord(start);
        
        cudaMemset(d_image, 0, newImageSize); 

        render<<<grid, block>>>(d_image, scale, centerX, centerY, WIDTH, HEIGHT);

        cudaDeviceSynchronize();

        // Timing STOP
        cudaEventRecord(stop);
        cudaEventSynchronize(stop);

        float milliseconds = 0.0f;
        cudaEventElapsedTime(&milliseconds, start, stop);

        cudaMemcpy(h_image, d_image, newImageSize, cudaMemcpyDeviceToHost);

        fwrite(h_image, 1, newImageSize, stdout);
        fflush(stdout);

        fprintf(stderr, "Frame render time: %.3f ms\n", milliseconds);
        fflush(stderr);
    }

    // Ressourcen freigeben, wenn die Schleife beendet ist
    if (d_image) {
        cudaFree(d_image);
    }
    if (h_image) {
        free(h_image);
    }
    cudaEventDestroy(start);
    cudaEventDestroy(stop);

    fprintf(stderr, "CUDA Backend clean exit\n");
    fflush(stderr);

    return 0;
}
