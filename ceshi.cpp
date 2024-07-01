#include <stdio.h>
#include <sched.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include<sys/time.h>

class Histogram {
 public:
  Histogram() { }
  ~Histogram() { }
  void Clear();
  void Add(double value);
  void Merge(const Histogram& other);
  double Median() const;
  double Percentile(double p) const;
  double Average() const;
  double StandardDeviation() const;
 private:
  double min_;
  double max_;
  double num_;
  double sum_;
  double sum_squares_;

  enum { kNumBuckets = 154 };
  static const double kBucketLimit[kNumBuckets];
  double buckets_[kNumBuckets];
};
const double Histogram::kBucketLimit[kNumBuckets] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 16, 18, 20, 25, 30, 35, 40, 45,
  50, 60, 70, 80, 90, 100, 120, 140, 160, 180, 200, 250, 300, 350, 400, 450,
  500, 600, 700, 800, 900, 1000, 1200, 1400, 1600, 1800, 2000, 2500, 3000,
  3500, 4000, 4500, 5000, 6000, 7000, 8000, 9000, 10000, 12000, 14000,
  16000, 18000, 20000, 25000, 30000, 35000, 40000, 45000, 50000, 60000,
  70000, 80000, 90000, 100000, 120000, 140000, 160000, 180000, 200000,
  250000, 300000, 350000, 400000, 450000, 500000, 600000, 700000, 800000,
  900000, 1000000, 1200000, 1400000, 1600000, 1800000, 2000000, 2500000,
  3000000, 3500000, 4000000, 4500000, 5000000, 6000000, 7000000, 8000000,
  9000000, 10000000, 12000000, 14000000, 16000000, 18000000, 20000000,
  25000000, 30000000, 35000000, 40000000, 45000000, 50000000, 60000000,
  70000000, 80000000, 90000000, 100000000, 120000000, 140000000, 160000000,
  180000000, 200000000, 250000000, 300000000, 350000000, 400000000,
  450000000, 500000000, 600000000, 700000000, 800000000, 900000000,
  1000000000, 1200000000, 1400000000, 1600000000, 1800000000, 2000000000,
  2500000000.0, 3000000000.0, 3500000000.0, 4000000000.0, 4500000000.0,
  5000000000.0, 6000000000.0, 7000000000.0, 8000000000.0, 9000000000.0,
  1e200,
};

void Histogram::Clear() {
  min_ = kBucketLimit[kNumBuckets-1];
  max_ = 0;
  num_ = 0;
  sum_ = 0;
  sum_squares_ = 0;
  for (int i = 0; i < kNumBuckets; i++) {
    buckets_[i] = 0;
  }
}

void Histogram::Add(double value) {
  // Linear search is fast enough for our usage in db_bench
  int b = 0;
  while (b < kNumBuckets - 1 && kBucketLimit[b] <= value) {
    b++;
  }
  buckets_[b] += 1.0;
  if (min_ > value) min_ = value;
  if (max_ < value) max_ = value;
  num_++;
  sum_ += value;
  sum_squares_ += (value * value);
}

void Histogram::Merge(const Histogram& other) {
  if (other.min_ < min_) min_ = other.min_;
  if (other.max_ > max_) max_ = other.max_;
  num_ += other.num_;
  sum_ += other.sum_;
  sum_squares_ += other.sum_squares_;
  for (int b = 0; b < kNumBuckets; b++) {
    buckets_[b] += other.buckets_[b];
  }
}

double Histogram::Median() const {
  return Percentile(99.0);
}

double Histogram::Percentile(double p) const {
  double threshold = num_ * (p / 100.0);
  double sum = 0;
  for (int b = 0; b < kNumBuckets; b++) {
    sum += buckets_[b];
    if (sum >= threshold) {
      // Scale linearly within this bucket
      double left_point = (b == 0) ? 0 : kBucketLimit[b-1];
      double right_point = kBucketLimit[b];
      double left_sum = sum - buckets_[b];
      double right_sum = sum;
      double pos = (threshold - left_sum) / (right_sum - left_sum);
      double r = left_point + (right_point - left_point) * pos;
      if (r < min_) r = min_;
      if (r > max_) r = max_;
      return r;
    }
  }
  return max_;
}


//intel 13代处理器 0-5是大核，6-9是小核；

int nums=0;                             //核的总数
pthread_mutex_t mutex;
//block 1
const int kNumBuckets=154;
struct attr{                              //线程创建函数的参数结构体，传递要绑定的核数和总的核数；
        long long start;
        long long finish;
        int core;
        double duration_time;
        int done;
        Histogram hist_;
    };

long long get_currenttime(){
    struct timeval enqueue_time;
    gettimeofday(&enqueue_time, NULL);
    return enqueue_time.tv_sec*1000000L+enqueue_time.tv_usec;
}
void *thread_work_func(void *dev)
{

        struct attr *info=(struct attr*)dev;

        int index=info->core;
        //0-5是大核，6-9是小核；  
        int count=12000;
        int x=2000;
        /*if(index<=5){
            x=2000;
            count=12000;
        }  
        else{
            x=4000;    
            count=20000;
        }*/
        cpu_set_t mask;                   //cpu核的集合；
        CPU_ZERO(&mask);                  //将cpu清空；
        CPU_SET(index,&mask);
        pthread_t id = pthread_self();    //获取本线程id；
        if(pthread_setaffinity_np(id,sizeof(cpu_set_t), &mask)!=0){    //将线程和核心绑定；
                printf("warning: could not set CPU affinity, continuing...\n");
                }
        //printf("线程进入循环！\n")
        //运行一定的时间；
        long long start_time=get_currenttime();
        long long finish_time=start_time+(info->duration_time*1000000);
        info->start=start_time;
        long long cur_time=start_time;
        while(cur_time<finish_time){
            pthread_mutex_lock(&mutex);
            for(int i=0;i<x;i++);
            pthread_mutex_unlock(&mutex); 
            long long end=get_currenttime();
            info->hist_.Add(end-cur_time);
            info->done++;  
            for(int i=0;i<count;i++);                   //nop指令模仿非临界区的作业；

            cur_time=get_currenttime();
       
        }
        info->finish=get_currenttime();
        return NULL;
}

int main(int argc,char **argv)
{
    int res;
    pthread_mutex_init(&mutex,NULL);           //初始化互斥锁
    nums=sysconf(_SC_NPROCESSORS_CONF);        //获取核数；
    printf("the system has %d cores!\n",nums);
    srand ( time(NULL) );
    double duration_time=10;
    int k=1;
    while(k<=10){
        pthread_t thread[k];                    
        int cores[k];
        struct attr attr_t[k];
        for(int i=0;i<k;i++){
            cores[i]=i;
            attr_t[i].core=cores[i];
            attr_t[i].duration_time=10;
            attr_t[i].done=0;
            attr_t[i].hist_.Clear();
            res=pthread_create(&thread[i],NULL,thread_work_func,&attr_t[i]);
                if(res!=0){
                    printf("error:can not create thread!\n");
                    return -1;
                }
        }
        /*等待线程的结束*/
        for(int i=0;i<k;i++) {
            pthread_join(thread[i],NULL);
        }
        for(int i=0;i<k;i++){
          printf("线程%d花费了%lld\n",i,attr_t[i].finish-attr_t[i].start);
        }
        for(int i=1;i<k;i++){
            if(attr_t[i].start<attr_t[0].start)
                attr_t[0].start=attr_t[i].start;
            if(attr_t[i].finish>attr_t[0].start)
                attr_t[0].finish=attr_t[i].finish;
            attr_t[0].done+=attr_t[i].done;
            attr_t[0].hist_.Merge(attr_t[i].hist_);
        }
        printf("尾部延时是%f",attr_t[0].hist_.Median());
        double elapse=(attr_t[0].finish-attr_t[0].start)*1.0;
        printf("\n");
        printf("%d个线程并发执行%f微秒执行了%d次，吞吐量为：%f\n",k,elapse,attr_t[0].done,attr_t[0].done*1.0/elapse);
        printf("\n");
        if(k==1)
          k++;
        else 
          k+=2;
    }
    //销毁互斥锁
    pthread_mutex_destroy(&mutex);

    return 0;
}

