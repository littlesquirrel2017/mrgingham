#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>

#include "point.hh"
#include "mrgingham_internal.h"

extern "C"
{
#include "ChESS.h"
}

// comment this!
#define CONSTANCY_WINDOW_R                  5
#define STDEV_THRESHOLD                     25
#define VARIANCE_THRESHOLD                  (STDEV_THRESHOLD*STDEV_THRESHOLD)
#define PYR_EXTRA_BLUR_R                    1

#define RESPONSE_MIN_PEAK_THRESHOLD         200
#define RESPONSE_MIN_THRESHOLD              20
#define RESPONSE_MIN_THRESHOLD_RATIO_OF_MAX(xmax) (((uint16_t)(xmax)) >> 4)

using namespace mrgingham;


static bool high_variance( int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t* image )
{
    if(x-CONSTANCY_WINDOW_R < 0 || x+CONSTANCY_WINDOW_R >= w ||
       y-CONSTANCY_WINDOW_R < 0 || y+CONSTANCY_WINDOW_R >= h )
    {
        // I give up on edges
        return false;
    }

    // I should be able to do this with opencv, but it's way too much of a pain
    // in my ass, so I do it myself
    int32_t sum = 0;
    for(int dy = -CONSTANCY_WINDOW_R; dy <=CONSTANCY_WINDOW_R; dy++)
        for(int dx = -CONSTANCY_WINDOW_R; dx <=CONSTANCY_WINDOW_R; dx++)
        {
            uint8_t val = image[ x+dx + (y+dy)*w ];
            sum += (int32_t)val;
        }

    int32_t mean = sum / ((1 + 2*CONSTANCY_WINDOW_R)*
                          (1 + 2*CONSTANCY_WINDOW_R));
    int32_t sum_deviation_sq = 0;
    for(int dy = -CONSTANCY_WINDOW_R; dy <=CONSTANCY_WINDOW_R; dy++)
        for(int dx = -CONSTANCY_WINDOW_R; dx <=CONSTANCY_WINDOW_R; dx++)
        {
            uint8_t val = image[ x+dx + (y+dy)*w ];
            int32_t deviation = (int32_t)val - mean;
            sum_deviation_sq += deviation*deviation;
        }

    int32_t var = sum_deviation_sq / ((1 + 2*CONSTANCY_WINDOW_R)*
                                      (1 + 2*CONSTANCY_WINDOW_R));

    // // to show the variances and empirically find the threshold
    // printf("%d %d %d\n", x, y, var);
    // return false;

    return var > VARIANCE_THRESHOLD;
}

// The point-list data structure for the connected-component traversal
struct xy_t { int16_t x, y; };
struct xylist_t
{
    struct xy_t* xy;
    int N;
};

static struct xylist_t xylist_alloc()
{
    struct xylist_t l = {};

    // start out large-enough for most use cases (should have connected
    // components with <10 pixels generally). Will realloc if really needed
    l.xy = (struct xy_t*)malloc( 128 * sizeof(struct xy_t) );

    return l;
}
static void xylist_reset_with(struct xylist_t* l, int16_t x, int16_t y)
{
    l->N = 1;
    l->xy[0].x = x;
    l->xy[0].y = y;
}
static void xylist_free(struct xylist_t* l)
{
    free(l->xy);
    l->N = -1;
}
static void xylist_push(struct xylist_t* l, int16_t x, int16_t y)
{
    l->N++;
    l->xy = (struct xy_t*)realloc(l->xy, l->N * sizeof(struct xy_t)); // no-op most of the time

    l->xy[l->N-1].x = x;
    l->xy[l->N-1].y = y;
}
static bool xylist_pop(struct xylist_t* l, int16_t *x, int16_t *y)
{
    if(l->N <= 0)
        return false;

    *x = l->xy[ l->N-1 ].x;
    *y = l->xy[ l->N-1 ].y;
    l->N--;
    return true;
}

static void mark_invalid(int16_t x, int16_t y, int16_t w, int16_t h, int16_t* d)
{
    d[x + y*w] = 0;
}


struct connected_component_t
{
    uint64_t sum_w_x, sum_w_y, sum_w;
    int N;

    // I keep track of the position and magnitude of the peak, and I reject all
    // points whose response is smaller than some (small) ratio of the max. Note
    // that the max is updated as I go, so it's possible to accumulate some
    // points that have become invalid (because the is_valid threshold has moved
    // with the max). However, since I weigh everything by the response value,
    // this won't be a strong effect: the incorrectly-accumulated points will
    // have a small weight
    uint16_t x_peak, y_peak;
    int16_t  response_max;
};
struct connected_component_t connected_component_init(void)
{
    struct connected_component_t c = {};
    return c;
}

static bool is_valid(int16_t x, int16_t y, int16_t w, int16_t h, const int16_t* d,
                     const struct connected_component_t* c)
{
    int16_t response = d[x + y*w];

    return
        response > RESPONSE_MIN_THRESHOLD &&
        (c == NULL || response > RESPONSE_MIN_THRESHOLD_RATIO_OF_MAX(c->response_max));
}
static void accumulate(int16_t x, int16_t y, int16_t w, int16_t h, const int16_t* d,
                       struct connected_component_t* c)
{
    int16_t response = d[x + y*w];
    if( response > c->response_max)
    {
        c->response_max = response;
        c->x_peak       = x;
        c->y_peak       = y;
    }

    c->sum_w_x += response * x;
    c->sum_w_y += response * y;
    c->sum_w   += response;
    c->N++;


    // // to show the responses and empirically find the threshold
    // printf("%d %d %d\n", x, y, response);

}
static bool connected_component_is_valid(const struct connected_component_t* c,

                                         int16_t w, int16_t h,
                                         const uint8_t* image)
{
    // We're looking at a candidate peak. I don't want to find anything
    // inside a chessboard square, which the detector does sometimes. I
    // can detect this condition by looking at a local variance of the
    // original image at this point. The image will be relatively
    // constant in a chessboard square, and I throw out this candidate
    // then
    return
        c->N > 1                                      &&
        c->response_max > RESPONSE_MIN_PEAK_THRESHOLD &&
        high_variance(c->x_peak, c->y_peak,
                      w,h, image);
}
static void follow_connected_component(struct xylist_t* l,
                                       int16_t w, int16_t h, int16_t* d,

                                       const uint8_t* image,
                                       std::vector<Point>* points,
                                       bool dodump )
{
    struct connected_component_t c = connected_component_init();

    int16_t x, y;
    while( xylist_pop(l, &x, &y))
    {
        if(!is_valid(x,y,w,h,d, &c))
            continue;

        accumulate  (x,y,w,h,d, &c);
        mark_invalid(x,y,w,h,d);

        if( x < w-1) xylist_push(l, x+1, y);
        if( x > 0  ) xylist_push(l, x-1, y);
        if( y < h-1) xylist_push(l, x,   y+1);
        if( y > 0  ) xylist_push(l, x,   y-1);
    }

    if( connected_component_is_valid(&c, w,h,image) )
    {
        double x = (double)c.sum_w_x / (double)c.sum_w;
        double y = (double)c.sum_w_y / (double)c.sum_w;

        if( dodump )
            printf("%f %f\n", x, y);
        else
            points->push_back( Point((int)(0.5 + x * FIND_GRID_SCALE),
                                     (int)(0.5 + y * FIND_GRID_SCALE)));
    }
}

static void process_connected_components(int w, int h, int16_t* d,

                                         const uint8_t* image,
                                         std::vector<Point>* points,
                                         bool dodump )
{
    struct xylist_t l = xylist_alloc();

    for(int16_t y = 0; y<h; y++)
        for(int16_t x = 0; x<w; x++)
        {
            if( !is_valid(x,y,w,h,d, NULL) )
                continue;

            xylist_reset_with(&l, x, y);
            follow_connected_component(&l, w,h,d,
                                       image, points, dodump);
        }
}

bool find_chessboard_corners_from_image_array( std::vector<Point>* points,
                                               const cv::Mat& image,
                                               bool dodump )
{
    if( !image.isContinuous() )
    {
        fprintf(stderr, "%s:%d in %s(): I can only handle continuous arrays (stride == width) currently."
                " Sorry.\n", __FILE__, __LINE__, __func__);
        return false;
    }

    if( image.type() != CV_8U )
    {
        fprintf(stderr, "%s:%d in %s(): I can only handle CV_8U arrays currently."
                " Sorry.\n", __FILE__, __LINE__, __func__);
        return false;
    }

    const int w = image.cols;
    const int h = image.rows;
    cv::Mat response( h, w, CV_16S );

    uint8_t* imageData    = image.data;
    int16_t* responseData = (int16_t*)response.data;


    ChESS_response_5( responseData, imageData, w, h );

    // I set all responses <0 to "0". These are not valid as candidates, and
    // I'll use "0" to mean "visited" in the upcoming connectivity search
    for( int xy = 0; xy < w*h; xy++ )
        if(responseData[xy] < 0)
            responseData[xy] = 0;

    // I have responses. I
    //
    // - Find local peaks
    // - Ignore invalid local peaks
    // - Find center-of-mass of the region around the local peak

    // This serves both to throw away duplicate nearby points at the same corner
    // and to provide sub-pixel-interpolation for the corner location
    process_connected_components(w, h, responseData,
                                 (uint8_t*)image.data, points, dodump);
    return true;
}

bool find_chessboard_corners_from_image_file( std::vector<Point>* points,
                                              const char* filename,
                                              bool dodump )
{
    cv::Mat image = cv::imread(filename, CV_LOAD_IMAGE_GRAYSCALE);
    if( image.data == NULL )
    {
        fprintf(stderr, "%s:%d in %s(): Couldn't open image '%s'."
                " Sorry.\n", __FILE__, __LINE__, __func__, filename);
        return false;
    }

    return find_chessboard_corners_from_image_array( points, image, dodump );
}