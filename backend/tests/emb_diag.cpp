// Standalone diagnostic: drive MNN's Embedding engine directly to isolate
// tokenizer vs graph issues in the BGE-M3 MNN conversion.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "llm/llm.hpp"

using namespace MNN::Transformer;
using namespace MNN::Express;

static void dump(const char *label, VARP v, const std::string &bin_path) {
  if (!v.get()) {
    std::printf("%s: NULL\n", label);
    return;
  }
  const auto *info = v->getInfo();
  int n = info ? (int)info->size : 0;
  const float *p = v->readMap<float>();
  std::printf("%s dim=%d first4:", label, n);
  for (int i = 0; i < 4 && i < n; ++i) std::printf(" %.6f", p[i]);
  std::printf("\n");
  if (!bin_path.empty() && n > 0) {
    std::ofstream f(bin_path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(p), (std::streamsize)(n * sizeof(float)));
  }
}

int main(int argc, char **argv) {
  const std::string dir = "C:/msys64/tmp/mnn-e2e/models/bge-m3/";
  auto *emb = Embedding::createEmbedding(dir);
  if (!emb) {
    std::printf("LOAD FAIL\n");
    return 1;
  }
  std::printf("loaded ok\n");

  // 1. What does MNN's tokenizer produce?
  auto ids1 = emb->tokenizer_encode("miniclaw local embedding quality test");
  std::printf("MNN-ids t1:");
  for (int i : ids1) std::printf(" %d", i);
  std::printf("\n");
  auto idsh = emb->tokenizer_encode("hello");
  std::printf("MNN-ids hello:");
  for (int i : idsh) std::printf(" %d", i);
  std::printf("\n");

  // 2. Explicit HF ids: t1 = [0,140301,19729,4000,6,55720,59725,31897,3034,2]
  std::vector<int> hf_t1 = {0, 140301, 19729, 4000, 6, 55720, 59725, 31897,
                            3034, 2};
  std::vector<int> hf_hello = {0, 33600, 31, 2};

  dump("hf-t1", emb->ids_embedding(hf_t1),
       "C:/msys64/tmp/mnn-e2e/emb_diag_t1.bin");
  dump("hf-hello", emb->ids_embedding(hf_hello),
       "C:/msys64/tmp/mnn-e2e/emb_diag_hello.bin");

  // 3. MNN-tokenized path (what the app actually uses)
  dump("mnn-t1", emb->txt_embedding("miniclaw local embedding quality test"),
       "C:/msys64/tmp/mnn-e2e/emb_diag_mnnt1.bin");

  // 4. More texts for the Python-side cosine comparison.
  const char *texts[] = {
      "hello",
      "The weather in Shanghai is rainy today.",
      "\u5929\u6c14\u771f\u4e0d\u9519\uff0c\u9002\u5408\u51fa\u6765\u6563\u6b65\u3002",  // Chinese
      "Quarterly earnings beat expectations; the CFO emphasized cost discipline.",
  };
  for (int i = 0; i < 4; ++i) {
    auto ids = emb->tokenizer_encode(texts[i]);
    std::printf("MNN-ids t%d:", i + 2);
    for (int x : ids) std::printf(" %d", x);
    std::printf("\n");
    char path[256];
    std::snprintf(path, sizeof(path), "C:/msys64/tmp/mnn-e2e/emb_diag_t%d.bin", i + 2);
    char label[16];
    std::snprintf(label, sizeof(label), "mnn-t%d", i + 2);
    dump(label, emb->txt_embedding(texts[i]), path);
  }

  return 0;
}
