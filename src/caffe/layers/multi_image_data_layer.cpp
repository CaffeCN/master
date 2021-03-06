#ifdef USE_OPENCV
#include <opencv2/core/core.hpp>

#include <fstream>  // NOLINT(readability/streams)
#include <iostream>  // NOLINT(readability/streams)
#include <string>
#include <utility>
#include <vector>
#include <boost/lexical_cast.hpp>  

#include "caffe/data_transformer.hpp"
#include "caffe/layers/base_data_layer.hpp"
#include "caffe/layers/multi_image_data_layer.hpp"
#include "caffe/util/benchmark.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"

namespace caffe {

template <typename Dtype>
MultiImageDataLayer<Dtype>::~MultiImageDataLayer<Dtype>() {
  this->StopInternalThread();
}

template <typename Dtype>
void MultiImageDataLayer<Dtype>::DataLayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  const int img_num    = this->layer_param_.multi_image_data_param().img_num();
  const int new_height = this->layer_param_.multi_image_data_param().new_height();
  const int new_width  = this->layer_param_.multi_image_data_param().new_width();
  const bool is_color  = this->layer_param_.multi_image_data_param().is_color();
  string root_folder   = this->layer_param_.multi_image_data_param().root_folder();

  CHECK((new_height == 0 && new_width == 0) ||
      (new_height > 0 && new_width > 0)) << "Current implementation requires "
      "new_height and new_width to be set at the same time.";
  // Read the file with filenames and labels
  const string& source = this->layer_param_.multi_image_data_param().source();
  LOG(INFO) << "Opening file " << source;
  std::ifstream infile(source.c_str());
  
  vector<string> filenames;
  int label;
  string line, result;
  stringstream sstr;
  while(getline(infile, line)) {
  	filenames.clear();
  	// stringstream sstr(line);
  	sstr.clear();
  	sstr << line;
  	int i = 0;
  	while(sstr >> result) { 
      if(i++ < img_num) {
      	filenames.push_back(root_folder + result);  // 这里已经加了root_folder
      } else {
      	label = boost::lexical_cast<int>(result);
      }
    }
  	lines_.push_back(std::make_pair(filenames, label));
  	sstr.str("");
  }
  vector<string>().swap(filenames); // 析构，释放内存
  
  if (this->layer_param_.multi_image_data_param().shuffle()) {
    // randomly shuffle data
    LOG(INFO) << "Shuffling data";
    const unsigned int prefetch_rng_seed = caffe_rng_rand();
    prefetch_rng_.reset(new Caffe::RNG(prefetch_rng_seed));
    ShuffleImages();
  }
  LOG(INFO) << "A total of " << lines_.size() << " images.";

  lines_id_ = 0;
  // Check if we would need to randomly skip a few data points
  if (this->layer_param_.multi_image_data_param().rand_skip()) {
    unsigned int skip = caffe_rng_rand() %
        this->layer_param_.multi_image_data_param().rand_skip();
    LOG(INFO) << "Skipping first " << skip << " data points.";
    CHECK_GT(lines_.size(), skip) << "Not enough points to skip";
    lines_id_ = skip;
  }
  // Read an image, and use it to initialize the top blob.
  vector<cv::Mat> cv_img_vec = ReadHyperImageToCVMatVec(lines_[lines_id_].first, img_num, new_height, new_width, is_color); 
  for(int i=0; i<img_num; i++)
  	CHECK(cv_img_vec[i].data) << "Could not load " << lines_[lines_id_].first[i];
  // Use data_transformer to infer the expected blob shape from a cv_image.
  vector<int> top_shape = this->data_transformer_->InferBlobShape(cv_img_vec[0]); // 下面会修正
  // vector<int> top_shape(batch_size, cv_img_vec[0]channels * img_num, new_height, new_width); // (n, c, h, w)
  
  this->transformed_data_.Reshape(top_shape);
  // Reshape prefetch_data and top[0] according to the batch_size.
  const int batch_size = this->layer_param_.multi_image_data_param().batch_size();
  CHECK_GT(batch_size, 0) << "Positive batch size required";
  // 修正top_shape
  top_shape[0] = batch_size;
  top_shape[1] = top_shape[1] * img_num; // 通道数 TODO: 如果图片通道数不一致怎么办？
  for (int i = 0; i < this->PREFETCH_COUNT; ++i) {
    this->prefetch_[i].data_.Reshape(top_shape);
  }
  top[0]->Reshape(top_shape);

  LOG(INFO) << "output data size: " << top[0]->num() << ","
      << top[0]->channels() << "," << top[0]->height() << ","
      << top[0]->width();
  // label
  vector<int> label_shape(1, batch_size);
  top[1]->Reshape(label_shape);
  for (int i = 0; i < this->PREFETCH_COUNT; ++i) {
    this->prefetch_[i].label_.Reshape(label_shape);
  }
}

template <typename Dtype>
void MultiImageDataLayer<Dtype>::ShuffleImages() {
  caffe::rng_t* prefetch_rng =
      static_cast<caffe::rng_t*>(prefetch_rng_->generator());
  shuffle(lines_.begin(), lines_.end(), prefetch_rng);
}

// This function is called on prefetch thread
template <typename Dtype>
void MultiImageDataLayer<Dtype>::load_batch(Batch<Dtype>* batch) {
  CPUTimer batch_timer;
  batch_timer.Start();
  double read_time = 0;
  double trans_time = 0;
  CPUTimer timer;
  CHECK(batch->data_.count());
  CHECK(this->transformed_data_.count());
  MultiImageDataParameter multi_image_data_param = this->layer_param_.multi_image_data_param();
  const int batch_size = multi_image_data_param.batch_size();
  const int img_num    = multi_image_data_param.img_num();
  const int new_height = multi_image_data_param.new_height();
  const int new_width  = multi_image_data_param.new_width();
  const bool is_color  = multi_image_data_param.is_color();
  string root_folder   = multi_image_data_param.root_folder();

  // Reshape according to the first image of each batch
  // on single input batches allows for inputs of varying dimension.
  vector<cv::Mat> cv_img_vec = ReadHyperImageToCVMatVec(lines_[lines_id_].first, img_num, 
                                                         new_height, new_width, is_color);
  for(int i=0; i<img_num; i++)
  	CHECK(cv_img_vec[i].data) << "Could not load " << lines_[lines_id_].first[i];
  // CHECK(cv_img.data) << "Could not load " << lines_[lines_id_].first;
  
  // Use data_transformer to infer the expected blob shape from a cv_img.
  vector<int> top_shape = this->data_transformer_->InferBlobShape(cv_img_vec[0]); // 下面会修正
  
  this->transformed_data_.Reshape(top_shape);
  // Reshape batch according to the batch_size.
  top_shape[0] = batch_size;
  top_shape[1] = top_shape[1] * img_num; // 通道数 TODO: 如果图片通道数不一致怎么办？
  batch->data_.Reshape(top_shape);

  Dtype* prefetch_data = batch->data_.mutable_cpu_data();
  Dtype* prefetch_label = batch->label_.mutable_cpu_data();

  // datum scales
  const int lines_size = lines_.size();
  for (int item_id = 0; item_id < batch_size; ++item_id) {
    // get a blob
    timer.Start();
    CHECK_GT(lines_size, lines_id_);
    vector<cv::Mat> cv_img_vec = ReadHyperImageToCVMatVec(lines_[lines_id_].first, img_num, 
                                                           new_height, new_width, is_color); 
    for(int i=0; i<img_num; i++)
  	  CHECK(cv_img_vec[i].data) << "Could not load " << lines_[lines_id_].first[i];
  	
    read_time += timer.MicroSeconds();
    timer.Start();
    // Apply transformations (mirror, crop...) to the image
    int offset = batch->data_.offset(item_id);
    this->transformed_data_.set_cpu_data(prefetch_data + offset);
    this->data_transformer_->Transform(cv_img_vec, &(this->transformed_data_), 1); //
    trans_time += timer.MicroSeconds();

    prefetch_label[item_id] = lines_[lines_id_].second;
    
    // go to the next iter
    lines_id_++;
    if (lines_id_ >= lines_size) {
      // We have reached the end. Restart from the first.
      DLOG(INFO) << "Restarting data prefetching from start.";
      lines_id_ = 0;
      if (this->layer_param_.multi_image_data_param().shuffle()) {
        ShuffleImages();
      }
    }
  }
  batch_timer.Stop();
  DLOG(INFO) << "Prefetch batch: " << batch_timer.MilliSeconds() << " ms.";
  DLOG(INFO) << "     Read time: " << read_time / 1000 << " ms.";
  DLOG(INFO) << "Transform time: " << trans_time / 1000 << " ms.";
}

INSTANTIATE_CLASS(MultiImageDataLayer);
REGISTER_LAYER_CLASS(MultiImageData);

}  // namespace caffe
#endif  // USE_OPENCV
