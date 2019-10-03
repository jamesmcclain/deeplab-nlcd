#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <gdal.h>

// Global variables
int N = 0;
int M = 0;
GDALDataType imagery_data_type = -1;
GDALDataType label_data_type = -1;
int operation_mode = 0;
int window_size = 0;
int band_count = 0;
int *bands = NULL;
int width = 0;
int height = 0;
uint64_t current = 0;

// Thread-related variables
pthread_t *threads = NULL;
GDALDatasetH *imagery_datasets = NULL;
GDALRasterBandH *imagery_first_bands = NULL;
GDALDatasetH *label_datasets = NULL;

// Slot-related variables
pthread_mutex_t *mutexes = NULL;
void **imagery_slots = NULL;
void **label_slots = NULL;
int *ready = NULL;

#define UNLOCK(index, useconds)                \
    {                                          \
        pthread_mutex_unlock(&mutexes[index]); \
        usleep(useconds);                      \
    }
#define UNLOCK_BREAK(index, useconds) \
    {                                 \
        UNLOCK(index, useconds)       \
        break;                        \
    }
#define UNLOCK_CONTINUE(index, useconds) \
    {                                    \
        UNLOCK(index, useconds)          \
        continue;                        \
    }

#define EMPTY_WINDOW (GDAL_DATA_COVERAGE_STATUS_EMPTY & GDALGetDataCoverageStatus(imagery_first_bands[id], window_size * x_offset, window_size * y_offset, window_size, window_size, 0, NULL))
#define BAD_TRAINING_WINDOW (((x_offset + y_offset) % 7) == 0)
#define BAD_EVALUATION_WINDOW (((x_offset + y_offset) % 7) != 0)

/**
 * Given a GDAL datat type, return the word length of that type.
 *
 * @param GDALDataType dt The data type
 * @return The word length
 */
int word_size(GDALDataType dt)
{
    switch (dt)
    {
    case GDT_Byte:
        return 1;
    case GDT_UInt16:
        return 2;
    case GDT_Int16:
        return 2;
    case GDT_UInt32:
        return 4;
    case GDT_Int32:
        return 4;
    case GDT_Float32:
        return 4;
    case GDT_Float64:
        return 8;
    case GDT_CInt16:
        return 4;
    case GDT_CInt32:
        return 8;
    case GDT_CFloat32:
        return 8;
    case GDT_CFloat64:
        return 16;
    default:
        assert(0);
    }
}

/**
 * Get the width of the dataset.
 *
 * @return The width of the dataset
 */
int get_width()
{
    return width;
}

/**
 * Get the height of the dataset.
 *
 * @return The height of the dataset
 */
int get_height()
{
    return height;
}

/**
 * Get an (inference) chip.  This can be used only if operation_mode 3
 * is active.
 *
 * @param imagery_buffer The return-pointer for the imagery data
 * @param x_offset The x-offset for the window (in pixels)
 * @param y_offset The y-offset for the window (in pixels)
 * @param attempts The maximum number of attempts to make to read the window
 * @return 1 for success, 0 for failure
 */
int get_inference_chip(void *imagery_buffer,
                       int x, int y,
                       int attempts)
{
    int id = 0;
    int x_offset = x / window_size;
    int y_offset = y / window_size;

    if ((operation_mode != 3) || EMPTY_WINDOW)
    {
        goto bad;
    }

    for (int i = 0; i < attempts; ++i)
    {
        CPLErr err = CE_None;

        // Read imagery
        err = GDALDatasetRasterIO(imagery_datasets[id], 0,
                                  x, y, window_size, window_size,
                                  imagery_buffer,
                                  window_size, window_size,
                                  imagery_data_type, band_count, bands,
                                  0, 0, 0);
        if (err != CE_None)
        {
            continue;
        }
        goto good;
    }

bad:
    memset(imagery_buffer, 0, word_size(imagery_data_type) * band_count * window_size * window_size);
    return 0;

good:
    return 1;
}

/**
 * Get the next available window.
 *
 * @param imagery_buffer The return-location for the imagery data
 * @param label_buffer The return-location for the label data
 */
void get_next(void *imagery_buffer, void *label_buffer)
{
    for (;; ++current)
    {
        int slot = current % M;

        if (pthread_mutex_trylock(&mutexes[slot]) == 0)
        {
            if (ready[slot] != 1)
            {
                pthread_mutex_unlock(&mutexes[slot]);
                continue;
            }
            else if (ready[slot] == 1)
            {
                memcpy(imagery_buffer, imagery_slots[slot], word_size(imagery_data_type) * band_count * window_size * window_size);
                if (label_buffer != NULL)
                {
                    memcpy(label_buffer, label_slots[slot], word_size(label_data_type) * 1 * window_size * window_size);
                }
                ready[slot] = 0;
                pthread_mutex_unlock(&mutexes[slot]);
                break;
            }
        }
    }
}

/**
 * The code behind the reader threads.
 *
 * @param _id The id of this particular thread
 * @return Unused
 */
void *reader(void *_id)
{
    uint64_t id = (uint64_t)_id;
    int x_offset = 0;
    int y_offset = 0;
    int slot = -1;
    CPLErr err = CE_None;
    unsigned int state = (unsigned long)id;

    while (operation_mode == 1 || operation_mode == 2)
    {
        // Get a suitable training or evaluation window
        if (operation_mode == 1) // Training chip
        {
            x_offset = y_offset = 0;
            while (BAD_TRAINING_WINDOW || EMPTY_WINDOW)
            {
                x_offset = rand_r(&state) % (width / window_size);
                y_offset = rand_r(&state) % (height / window_size);
            }
        }
        else if (operation_mode == 2) // Evaluation chip
        {
            // XXX should not be random
            x_offset = 0;
            y_offset = 1;
            while (BAD_EVALUATION_WINDOW || EMPTY_WINDOW)
            {
                x_offset = rand_r(&state) % (width / window_size);
                y_offset = rand_r(&state) % (height / window_size);
            }
        }
        x_offset *= window_size;
        y_offset *= window_size;

        // Find an unused data slot
        for (slot = rand_r(&state) % M; (operation_mode == 1 || operation_mode == 2); slot = (slot + 1) % M)
        {
            // If slot is unlocked and slot is empty, read
            if ((pthread_mutex_trylock(&mutexes[slot]) == 0) && (ready[slot] == 0))
            {
                goto read_things;
            }
            UNLOCK_CONTINUE(slot, 100);
        }

        // If search for slot terminated because the mode changed,
        // break out of the loop
        if (operation_mode != 1 && operation_mode != 2)
        {
            UNLOCK_BREAK((slot + M - 1) % M, 0);
        }

    read_things:

        // Read imagery
        err = GDALDatasetRasterIO(imagery_datasets[id], 0,
                                  x_offset, y_offset, window_size, window_size,
                                  imagery_slots[slot],
                                  window_size, window_size,
                                  imagery_data_type, band_count, bands,
                                  0, 0, 0);
        if (err != CE_None)
        {
            UNLOCK_CONTINUE(slot, 1000)
        }

        // Read labels
        if (label_datasets[id] != NULL)
        {
            err = GDALDatasetRasterIO(label_datasets[id], 0,
                                      x_offset, y_offset, window_size, window_size,
                                      label_slots[slot],
                                      window_size, window_size,
                                      label_data_type, 1, NULL,
                                      0, 0, 0);
            if (err != CE_None)
            {
                UNLOCK_CONTINUE(slot, 1000)
            }
        }

        // The slot is now ready for reading
        ready[slot] = 1;

        // Done
        UNLOCK(slot, 1000)
    }

    return NULL;
}

/**
 * Initialize the library.
 */
void init()
{
    GDALAllRegister();
}

/**
 * Deinitialize the library.
 */
void deinit()
{
    GDALDestroy();
}

/**
 * Given imagery and label filenames, start the reader threads.
 *
 * @param _N The number of reader threads to create
 * @param _M The number of slots
 * @param imagery_filename The filename containing the imagery
 * @param label_filename The filename containing the labels
 * @param _operation_mode 1 for training mode, 2 for evaluation mode, 3 for inference mode
 * @param _window_size The desired window size
 * @param _band_count The number of bands
 * @param _bands An array of integers containing the desired bands
 */
void start(int _N,
           int _M,
           const char *imagery_filename, const char *label_filename,
           GDALDataType _imagery_data_type, GDALDataType _label_data_type,
           int _operation_mode,
           int _window_size,
           int _band_count, int *_bands)
{
    // Set globals
    N = _N;
    M = _M;
    imagery_data_type = _imagery_data_type;
    label_data_type = _label_data_type;
    operation_mode = _operation_mode;
    window_size = _window_size;
    band_count = _band_count;
    bands = (int *)malloc(sizeof(int) * band_count);
    memcpy(bands, _bands, sizeof(int) * band_count);

    // Per-thread arrays (except for width and height)
    imagery_datasets = (GDALDatasetH *)malloc(sizeof(GDALDatasetH) * N);
    imagery_first_bands = (GDALRasterBandH *)malloc(sizeof(GDALRasterBandH) * N);
    label_datasets = (GDALDatasetH *)malloc(sizeof(GDALDatasetH) * N);
    imagery_datasets[0] = GDALOpen(imagery_filename, GA_ReadOnly);
    width = GDALGetRasterXSize(imagery_datasets[0]);
    height = GDALGetRasterYSize(imagery_datasets[0]);
    threads = (pthread_t *)malloc(sizeof(pthread_t) * N);

    // Per-slot arrays
    mutexes = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t) * M);
    imagery_slots = malloc(sizeof(void *) * M);
    label_slots = malloc(sizeof(void *) * M);
    ready = calloc(M, sizeof(int));

    // Fill arrays, start threads
    for (int64_t i = 0; i < M; ++i)
    {
        imagery_slots[i] = malloc(word_size(imagery_data_type) * band_count * window_size * window_size);
        label_slots[i] = malloc(word_size(label_data_type) * 1 * window_size * window_size);
        pthread_mutex_init(&mutexes[i], NULL);
    }
    for (int64_t i = 0; i < N; ++i)
    {
        if (i != 0)
        {
            imagery_datasets[i] = GDALOpen(imagery_filename, GA_ReadOnly);
        }
        imagery_first_bands[i] = GDALGetRasterBand(imagery_datasets[i], 1);
        if (label_filename != NULL)
        {
            label_datasets[i] = GDALOpen(label_filename, GA_ReadOnly);
        }
        else
        {
            label_datasets[i] = NULL;
        }

        pthread_create(&threads[i], NULL, reader, (void *)i);
    }

    return;
}

/**
 * Stop the reader threads.
 */
void stop()
{
    operation_mode = 0;
    for (int i = 0; i < N; ++i)
    {
        pthread_join(threads[i], NULL);
        GDALClose(imagery_datasets[i]);
        if (label_datasets[i] != NULL)
        {
            GDALClose(label_datasets[i]);
        }
    }
    for (int i = 0; i < M; ++i)
    {
        free(imagery_slots[i]);
        free(label_slots[i]);
        ready[i] = 0;
    }

    free(bands);
    free(threads);
    free(mutexes);
    free(imagery_datasets);
    free(imagery_first_bands);
    free(label_datasets);
    free(imagery_slots);
    free(label_slots);
    free(ready);

    N = M = 0;
    bands = NULL;
    threads = NULL;
    mutexes = NULL;
    imagery_datasets = NULL;
    imagery_first_bands = NULL;
    label_datasets = NULL;
    imagery_slots = NULL;
    label_slots = NULL;
    ready = NULL;
}