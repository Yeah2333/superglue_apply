#include "superglue.h"
#include "spdlog/spdlog.h"

SuperGlue::SuperGlue(const YAML::Node &glue_config) : image_rows_(glue_config["image_rows"].as<int>()),
                                                      image_cols_(glue_config["image_cols"].as<int>()),
                                                      sinkhorn_iterations_(glue_config["sinkhorn_iterations"].as<int>()),
                                                      weight_(glue_config["weight"].as<std::string>())

{
    if (torch::cuda::is_available())
    {
        device_ = torch::Device(torch::kCUDA);
        spdlog::info("CUDA is available!");
    }
    else
    {
        spdlog::warn("CUDA is not available!");
    }
    assert(weight_ == "outdoor" || weight_ == "indoor");
    if (weight_ == "indoor")
    {
        module_ = std::make_shared<torch::jit::script::Module>(
            torch::jit::load(
                workspace +
                    "../superglue/models/model/SuperGlue_indoor.pt",
                device_));
        spdlog::info("Loaded SuperGlue model ('indoor' weights)");
    }

    else
    {
        module_ = std::make_shared<torch::jit::script::Module>(
            torch::jit::load(
                workspace +
                    "../superglue/models/model/SuperGlue_outdoor.pt",
                device_));
        spdlog::info("Loaded SuperGlue model ('outdoor' weights)");
    }
    assert(module_ != nullptr);
    spdlog::info("Load model successful!");
}

auto SuperGlue::logSinkhornIterations(torch::Tensor &Z, torch::Tensor &log_mu, torch::Tensor &log_nu, const int iters)
{
    auto u = torch::zeros_like(log_mu);
    auto v = torch::zeros_like(log_nu);
    for (auto i = 0; i < iters; ++i)
    {
        u = log_mu - torch::logsumexp(Z + v.unsqueeze(1), 2);
        v = log_nu - torch::logsumexp(Z + u.unsqueeze(2), 1);
    }
    return Z + u.unsqueeze(2) + v.unsqueeze(1);
}

auto SuperGlue::logOptimalTransport(const torch::Tensor &&scores, torch::Tensor &&alpha, const int iters)
{
    int b = scores.size(0);
    int m = scores.size(1);
    int n = scores.size(2);
    auto one = torch::ones(1).squeeze();
    auto ms = (m * one).to(scores);
    auto ns = (n * one).to(scores);

    auto bins0 = alpha.expand({b, m, 1});
    auto bins1 = alpha.expand({b, 1, n});
    alpha = alpha.expand({b, 1, 1});
    auto couplings = torch::cat({torch::cat({scores, bins0}, -1),
                                 torch::cat({bins1, alpha}, -1)},
                                1);
    auto norm = -(ms + ns).log();
    auto log_mu = torch::cat({norm.expand(m), ns.log().unsqueeze_(0) + norm});
    auto log_nu = torch::cat({norm.expand(n), ms.log().unsqueeze_(0) + norm});
    log_mu = log_mu.unsqueeze(0).expand({b, -1});
    log_nu = log_nu.unsqueeze(0).expand({b, -1});
    auto Z = logSinkhornIterations(couplings, log_mu, log_nu, iters);
    Z = Z - norm;
    return Z;
}

auto SuperGlue::arangeLike(const torch::Tensor &x, const int dim)
{
    auto a = torch::arange(x.size(dim));
    std::cout << a << std::endl;
    return a;
}

void SuperGlue::match(std::vector<cv::KeyPoint> &kpts0, std::vector<cv::KeyPoint> &kpts1, cv::Mat &desc0, cv::Mat &desc1)
{
    cv::Mat kpts_mat0(kpts0.size(), 2, CV_32F); // [n_keypoints, 2]  (y, x)
    cv::Mat kpts_mat1(kpts1.size(), 2, CV_32F); // [n_keypoints, 2]  (y, x)
    cv::Mat scores_mat0(kpts0.size(), 1, CV_32F);
    cv::Mat scores_mat1(kpts1.size(), 1, CV_32F);

#ifdef USE_OPENMP
#pragma omp parallel for
#endif
    for (size_t i = 0; i < kpts0.size(); ++i)
    {
        kpts_mat0.at<float>(i, 0) = static_cast<float>(kpts0[i].pt.y);
        kpts_mat0.at<float>(i, 1) = static_cast<float>(kpts0[i].pt.x);
        scores_mat0.at<float>(i) = static_cast<float>(kpts0[i].response);
    }
#ifdef USE_OPENMP
#pragma omp parallel for
#endif
    for (size_t i = 0; i < kpts1.size(); ++i)
    {
        kpts_mat1.at<float>(i, 0) = static_cast<float>(kpts1[i].pt.y);
        kpts_mat1.at<float>(i, 1) = static_cast<float>(kpts1[i].pt.x);
        scores_mat1.at<float>(i) = static_cast<float>(kpts1[i].response);
    }

    auto kpts0_tensor = torch::from_blob(kpts_mat0.data, {static_cast<long>(kpts0.size()), 2}, torch::kFloat).to(device_);
    auto kpts1_tensor = torch::from_blob(kpts_mat1.data, {static_cast<long>(kpts1.size()), 2}, torch::kFloat).to(device_);
    auto scores0_tensor = torch::from_blob(scores_mat0.data, {1, static_cast<long>(kpts0.size())}, torch::kFloat).to(device_);
    auto scores1_tensor = torch::from_blob(scores_mat1.data, {1, static_cast<long>(kpts1.size())}, torch::kFloat).to(device_);
    auto descriptors0 = torch::from_blob(desc0.data, {1, desc0.cols, desc0.rows}, torch::kFloat).to(device_);
    auto descriptors1 = torch::from_blob(desc1.data, {1, desc1.cols, desc1.rows}, torch::kFloat).to(device_);
#ifdef DEBUG
    std::cout << "kpts0_tensor:" << kpts0_tensor.sizes() << std::endl;
    std::cout << "kpts1_tensor:" << kpts1_tensor.sizes() << std::endl;
    std::cout << "scores0_tensor:" << scores0_tensor.sizes() << std::endl;
    std::cout << "scores1_tensor:" << scores1_tensor.sizes() << std::endl;
    std::cout << "descriptors0:" << descriptors0.sizes() << std::endl;
    std::cout << "descriptors1:" << descriptors1.sizes() << std::endl;
#endif
    auto kpts0_t = normalizeKeypoints(kpts0_tensor);
    auto kpts1_t = normalizeKeypoints(kpts1_tensor);
    std::cout << "kpts0_t:" << kpts0_t.sizes() << std::endl;
    std::cout << "kpts1_t:" << kpts1_t.sizes() << std::endl;

    torch::Dict<std::string, torch::Tensor> data;
    data.insert("keypoints0", kpts0_t);
    data.insert("keypoints1", kpts1_t);
    data.insert("scores0", scores0_tensor);
    data.insert("scores1", scores1_tensor);
    data.insert("descriptors0", scores0_tensor);
    data.insert("descriptors1", scores1_tensor);
#ifdef DEBUG
    auto start = std::chrono::steady_clock::now();
#endif
    auto out = module_->forward({data}).toTuple();

    auto scores = logOptimalTransport(out->elements()[0].toTensor(), out->elements()[1].toTensor(), sinkhorn_iterations_);
#ifdef DEBUG
    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed_seconds = end - start;
    std::cout << "SuperpGlue module elapsed time: " << elapsed_seconds.count() << "s\n";
#endif
    auto [indices0, values0] = scores.slice(1, 0, scores[0].size(0) - 1).slice(2, 0, scores[0].size(1) - 1).max(2);
    auto [indices1, values1] = scores.slice(1, 0, scores[0].size(0) - 1).slice(2, 0, scores[0].size(1) - 1).max(1);
    std::cout << indices1 << std::endl;
    auto aa = indices0.gather(1, indices1);
    std::cout << aa.sizes() << std::endl;
    // auto mutual0 = torch::arange(indices0.size(1)).unsqueeze(0) == indices0.gather(1, indices0);
    // auto mutual1 = torch::arange(indices1.size(1)).unsqueeze(0) == indices1.gather(1, indices1);
    auto zero = torch::zeros(1);
    std::cout << zero.sizes() << std::endl;
}

torch::Tensor SuperGlue::normalizeKeypoints(torch::Tensor &kpts)
{
    auto one = torch::ones(1).to(kpts);
    auto size = torch::stack({one * image_cols_, one * image_rows_}, 1);
    auto center = size / 2;
    auto scaling = std::get<1>(size.max(1, true)) * 1.7;
    // #ifdef DEBUG
    //     std::cout << one.sizes() << std::endl;
    //     std::cout << size.sizes() << std::endl;
    //     std::cout << size << std::endl;
    //     std::cout << std::get<0>(size.max(1, true)) << std::endl;
    //     std::cout << std::get<1>(size.max(1, true)) << std::endl;
    // #endif
    return ((kpts - center) / scaling).unsqueeze_(0);
}