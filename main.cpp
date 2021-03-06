#include "common.h"
#include "cudaUtility.h"
#include "mathFunctions.h"
#undef CHECK
#include "pluginImplement.h"
#include "tensorNet.h"
#include "loadImage.h"
#include <chrono>


const char* model  = "../../model/MobileNetSSD_deploy_iplugin.prototxt";
// const char* weight = "../../model/MobileNetSSD_deploy.caffemodel";
// const char* image = "../../testPic/test.jpg";
const char* weight = "../../nbn/no_bn.caffemodel";
const char* image = "../../testPic/test.jpg";

const char* INPUT_BLOB_NAME = "data";

const char* OUTPUT_BLOB_NAME = "detection_out";
static const uint32_t BATCH_SIZE = 1;


class Timer {
public:
    void tic() {
        start_ticking_ = true;
        start_ = std::chrono::high_resolution_clock::now();
    }
    void toc() {
        if (!start_ticking_)return;
        end_ = std::chrono::high_resolution_clock::now();
        start_ticking_ = false;
        t = std::chrono::duration<double, std::milli>(end_ - start_).count();
        //std::cout << "Time: " << t << " ms" << std::endl;
    }
    double t;
private:
    bool start_ticking_ = false;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_;
    std::chrono::time_point<std::chrono::high_resolution_clock> end_;
};


/* *
 * @TODO: unifiedMemory is used here under -> ( cudaMallocManaged )
 * */
float* allocateMemory(DimsCHW dims, char* info)
{
    float* ptr;
    size_t size;
    std::cout << "Allocate memory: " << info << std::endl;
    size = BATCH_SIZE * dims.c() * dims.h() * dims.w();
    assert(!cudaMallocManaged( &ptr, size*sizeof(float)));
    return ptr;
}


void loadImg( cv::Mat &input, int re_width, int re_height, float *data_unifrom,const float3 mean,const float scale )
{
    int i;
    int j;
    int line_offset;
    int offset_g;
    int offset_r;
    cv::Mat dst;

    unsigned char *line = NULL;
    float *unifrom_data = data_unifrom;

    cv::resize( input, dst, cv::Size( re_width, re_height ), (0.0), (0.0), cv::INTER_LINEAR );
    offset_g = re_width * re_height;
    offset_r = re_width * re_height * 2;
    for( i = 0; i < re_height; ++i )
    {
        line = dst.ptr< unsigned char >( i );
        line_offset = i * re_width;
        for( j = 0; j < re_width; ++j )
        {
            // b
            unifrom_data[ line_offset + j  ] = (( float )(line[ j * 3 ] - mean.x) * scale);
            // g
            unifrom_data[ offset_g + line_offset + j ] = (( float )(line[ j * 3 + 1 ] - mean.y) * scale);
            // r
            unifrom_data[ offset_r + line_offset + j ] = (( float )(line[ j * 3 + 2 ] - mean.z) * scale);
        }
    }
}

static char HELP[] = " [-h|--help] [--weight WEIGHT.caffemodel] [--model MODEL.prototxt] [--image IMAGE]";

#define ARG_IS(i,s) (strncmp(argv[1], (s), sizeof(s)) == 0)

static void jno_setup(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (ARG_IS(i, "-h") || ARG_IS(i, "--help")) {
            std::cout << basename(argv[0]) << HELP << std::endl;
            exit(EXIT_SUCCESS);
        } else if (ARG_IS(i, "--image")) {
            image = realpath(argv[++i], nullptr);
            std::cerr << "Image: " << image << std::endl;
        } else if (ARG_IS(i, "--model")) {
            model = realpath(argv[++i], nullptr);
            std::cerr << "Model: " << model << std::endl;
        } else if (ARG_IS(i, "--weight")) {
            weight = realpath(argv[++i], nullptr);
            std::cerr << "Weight: " << weight << std::endl;
        } else {
            std::cerr << "Bad flag '" << argv[i] << "'." << std::endl;
            exit(EXIT_FAILURE);
        }
    }
    char *p = strrchr(argv[0], '/'); *p = '\0'; chdir(argv[0]);
}

#define max(x,y) (((x) >= (y)) ? (x) : (y))
#define min(x,y) (((x) <= (y)) ? (x) : (y))

int main(int argc, char *argv[])
{
    jno_setup(argc, argv);

    std::cerr << "Image: " << image << std::endl;
    std::cerr << "Model: " << model << std::endl;
    std::cerr << "Weight: " << weight << std::endl;

    std::vector<std::string> output_vector = {OUTPUT_BLOB_NAME};
    TensorNet tensorNet;
    tensorNet.LoadNetwork(model,weight,INPUT_BLOB_NAME, output_vector,BATCH_SIZE);

    DimsCHW dimsData = tensorNet.getTensorDims(INPUT_BLOB_NAME);
    DimsCHW dimsOut  = tensorNet.getTensorDims(OUTPUT_BLOB_NAME);

    float* data    = allocateMemory( dimsData , (char*)"input blob");
    std::cout << "allocate data" << std::endl;
    float* output  = allocateMemory( dimsOut  , (char*)"output blob");
    std::cout << "allocate output" << std::endl;
    int height = 300;
    int width  = 300;

    cv::Mat frame,srcImg;

    void* imgCPU;
    void* imgCUDA;
    Timer timer;

    frame = cv::imread(image);
    srcImg = frame.clone();
    cv::resize(frame, frame, cv::Size(300,300));
    const size_t size = width * height * sizeof(float3);

    if( CUDA_FAILED( cudaMalloc( &imgCUDA, size)) )
    {
        cout <<"Cuda Memory allocation error occured."<<endl;
        return false;
    }

    void* imgData = malloc(size);
    memset(imgData,0,size);

    loadImg(frame,height,width,(float*)imgData,make_float3(127.5,127.5,127.5),0.007843);
    cudaMemcpyAsync(imgCUDA,imgData,size,cudaMemcpyHostToDevice);

    void* buffers[] = { imgCUDA, output };

    timer.tic();
    tensorNet.imageInference( buffers, output_vector.size() + 1, BATCH_SIZE);
    timer.toc();
    double msTime = timer.t;

    vector<vector<float> > detections;

    for (int k=0; k<100; k++)
    {
        if(output[7*k+1] == -1)
            break;
        float *out = &output[7*k];
        float xz = out[0];
        float klass = out[1];
        float confd = out[2];
        float xmin = min(out[3], out[5]);
        float ymin = min(out[4], out[6]);
        float xmax = max(out[5], out[3]);
        float ymax = max(out[6], out[4]);
        int x1 = static_cast<int>(xmin * srcImg.cols);
        int y1 = static_cast<int>(ymin * srcImg.rows);
        int x2 = static_cast<int>(xmax * srcImg.cols);
        int y2 = static_cast<int>(ymax * srcImg.rows);
        std::cout << "xz=" << xz << ", class=" << klass << ", confidence=" << confd << "\n"
            << "  bbox: " << xmin << ", " << ymin << ", " << xmax << ", " << ymax << "\n"
            << " image: " << srcImg.cols << " x " << srcImg.rows << "\n"
            << "  draw: " << x1 << ", " << y1 << ", " << x2 << ", " << y2 << "\n"
            << "expect: " << 222. << ", " << 182. << ", " << 252. << ", " << 191.
            << std::endl;
        cv::rectangle(srcImg,cv::Rect2f(cv::Point(x1,y1),cv::Point(x2,y2)),cv::Scalar(255,0,255),1);

    }
    cv::imshow("mobileNet",srcImg);
    cv::waitKey(0);
    free(imgData);
    cudaFree(imgCUDA);
    cudaFreeHost(imgCPU);
    cudaFree(output);
    tensorNet.destroy();
    return 0;
}

// vim: ai et ts=4 sts=4 sw=4
