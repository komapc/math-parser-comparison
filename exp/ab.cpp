#include "parser/evaluator.hpp"
#include <chrono>
#include <print>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <memory>
using namespace mp;
namespace mp { std::unique_ptr<IEvaluator> make_direct_v0(); std::unique_ptr<IEvaluator> make_direct_v1(); std::unique_ptr<IEvaluator> make_direct_v2(); std::unique_ptr<IEvaluator> make_direct_v3(); std::unique_ptr<IEvaluator> make_tree_v0(); std::unique_ptr<IEvaluator> make_tree_v1(); }
volatile double sink;
static std::string nest(int m){std::string s(m,'(');s+="1";for(int i=0;i<m;++i)s+=" + 1)";return s;}
static std::string sum(int m){std::string s="1";for(int i=1;i<m;++i){s+=(i%2)?" + ":" - ";s+=std::to_string(1+i%9);}return s;}
static std::string pw(int m){std::string s="3 ^ 2";for(int i=1;i<m;++i){int k=(i-1)/2;s+=(i%2)?" * ":" / ";s+=std::to_string(2+k%8)+" ^ "+std::to_string(2+k%3);}return s;}
static std::string tw(int m){std::string s="1";for(int i=0;i<m/2;++i)s+=" ^ 1";for(int i=m/2;i<m;++i)s+=" * 1";return s;}
int main(){
  std::vector<std::unique_ptr<IEvaluator>> evs;
  for(auto&e:all_evaluators()) if(std::string(e->name())=="direct-shunting-yard"||std::string(e->name())=="direct-reverse"||std::string(e->name())=="ast-arena"||std::string(e->name())=="multipass-reverse-fold") evs.push_back(std::move(e));
  evs.push_back(make_direct_v0()); evs.push_back(make_direct_v1()); evs.push_back(make_direct_v2()); evs.push_back(make_direct_v3()); evs.push_back(make_tree_v0()); evs.push_back(make_tree_v1());
  std::vector<std::string> corpus; {std::ifstream f("bench/corpus/n1000.txt"); for(std::string l;std::getline(f,l);) if(!l.empty()) corpus.push_back(l);}
  double cvars[26]={2,3,4,5};
  struct Sh{const char*n;std::vector<std::string> x;};
  std::vector<Sh> sh={{"nest",{nest(8192)}},{"sum",{sum(8192)}},{"pow",{pw(8192)}},{"tower",{tw(8192)}},{"corpus",corpus}};
  for(auto&s:sh){ double ref=evs[0]->eval(s.x[0],cvars); for(auto&e:evs) if(e->eval(s.x[0],cvars)!=ref){std::println("MISMATCH {} {}",e->name(),s.n);} }
  std::vector<std::vector<double>> best(sh.size(),std::vector<double>(evs.size(),1e30));
  for(int r=0;r<40;++r) for(size_t i=0;i<sh.size();++i) for(size_t k=0;k<evs.size();++k){
    auto t0=std::chrono::steady_clock::now(); double a=0; for(auto&x:sh[i].x) a+=evs[k]->eval(x,cvars); sink=a;
    best[i][k]=std::min(best[i][k],std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count()); }
  std::print("{:8}","shape"); for(auto&e:evs) std::print("{:>24}",e->name()); std::println("");
  for(size_t i=0;i<sh.size();++i){ std::print("{:8}",sh[i].n); for(size_t k=0;k<evs.size();++k) std::print("{:>24}",std::format("{:.3f}",best[i][k]*1e3)); std::println(""); }
}
