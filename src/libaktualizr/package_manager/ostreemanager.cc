#include "ostreemanager.h"

#include <unistd.h>
#include <cstdio>
#include <fstream>

#include <gio/gio.h>
#include <json/json.h>
#include <ostree-async-progress.h>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem.hpp>
#include <utility>

#include "libaktualizr/packagemanagerfactory.h"

#include "bootloader/bootloader.h"
#include "logging/logging.h"
#include "storage/invstorage.h"
#include "utilities/utils.h"

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
AUTO_REGISTER_PACKAGE_MANAGER(PACKAGE_MANAGER_OSTREE, OstreeManager);

static void aktualizr_progress_cb(OstreeAsyncProgress *progress, gpointer data) {
  auto *mt = static_cast<PullMetaStruct *>(data);
  if (mt->token != nullptr && !mt->token->canContinue()) {
    g_cancellable_cancel(mt->cancellable.get());
  }

  g_autofree char *status = ostree_async_progress_get_status(progress);
  guint scanning = ostree_async_progress_get_uint(progress, "scanning");
  guint outstanding_fetches = ostree_async_progress_get_uint(progress, "outstanding-fetches");
  guint outstanding_metadata_fetches = ostree_async_progress_get_uint(progress, "outstanding-metadata-fetches");
  guint outstanding_writes = ostree_async_progress_get_uint(progress, "outstanding-writes");
  guint n_scanned_metadata = ostree_async_progress_get_uint(progress, "scanned-metadata");

  if (status != nullptr && *status != '\0') {
    LOG_INFO << "ostree-pull: " << status;
  } else if (outstanding_fetches != 0) {
    guint fetched = ostree_async_progress_get_uint(progress, "fetched");
    guint metadata_fetched = ostree_async_progress_get_uint(progress, "metadata-fetched");
    guint requested = ostree_async_progress_get_uint(progress, "requested");
    if (scanning != 0 || outstanding_metadata_fetches != 0) {
      LOG_INFO << "ostree-pull: Receiving metadata objects: " << metadata_fetched
               << " outstanding: " << outstanding_metadata_fetches;
      if (mt->progress_cb) {
        mt->progress_cb(mt->target, "Receiving metadata objects", 0);
      }
    } else {
      guint calculated = (fetched * 100) / requested;
      if (calculated != mt->percent_complete) {
        mt->percent_complete = calculated;
        LOG_INFO << "ostree-pull: Receiving objects: " << calculated << "% ";
        if (mt->progress_cb) {
          mt->progress_cb(mt->target, "Receiving objects", calculated);
        }
      }
    }
  } else if (outstanding_writes != 0) {
    LOG_INFO << "ostree-pull: Writing objects: " << outstanding_writes;
  } else {
    LOG_INFO << "ostree-pull: Scanning metadata: " << n_scanned_metadata;
    if (mt->progress_cb) {
      mt->progress_cb(mt->target, "Scanning metadata", 0);
    }
  }
}

data::InstallationResult OstreeManager::pull(const boost::filesystem::path &sysroot_path,
                                             const std::string &ostree_server, const KeyManager &keys,
                                             const Uptane::Target &target, const api::FlowControlToken *token,
                                             OstreeProgressCb progress_cb, const char *alt_remote,
                                             boost::optional<std::unordered_map<std::string, std::string>> headers) {
  if (ostree_server.find("://") == std::string::npos) {
    return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed,
                                    "Invalid OSTree URI: must contain scheme (e.g., http://)");
  }

  if (!target.IsOstree()) {
    throw std::logic_error("Invalid type of Target, got " + target.type() + ", expected OSTREE");
  }

  const std::string refhash = target.sha256Hash();
  // NOLINTNEXTLINE(modernize-avoid-c-arrays, cppcoreguidelines-avoid-c-arrays, hicpp-avoid-c-arrays)
  const char *const commit_ids[] = {refhash.c_str()};
  GError *error = nullptr;
  GVariantBuilder builder;
  GVariant *options;
  GObjectUniquePtr<OstreeAsyncProgress> progress = nullptr;

  GObjectUniquePtr<OstreeSysroot> sysroot = OstreeManager::LoadSysroot(sysroot_path);
  GObjectUniquePtr<OstreeRepo> repo = LoadRepo(sysroot.get(), &error);
  if (error != nullptr) {
    LOG_ERROR << "Could not get OSTree repo";
    g_error_free(error);
    return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, "Could not get OSTree repo");
  }

  GHashTable *ref_list = nullptr;
  if (ostree_repo_list_commit_objects_starting_with(repo.get(), refhash.c_str(), &ref_list, nullptr, &error) != 0) {
    guint length = g_hash_table_size(ref_list);
    g_hash_table_destroy(ref_list);  // OSTree creates the table with destroy notifiers, so no memory leaks expected
    // should never be greater than 1, but use >= for robustness
    if (length >= 1) {
      LOG_DEBUG << "refhash already pulled";
      return data::InstallationResult(true, data::ResultCode::Numeric::kAlreadyProcessed, "Refhash was already pulled");
    }
  }
  if (error != nullptr) {
    g_error_free(error);
    error = nullptr;
  }

  if (alt_remote == nullptr) {
    std::string ostree_remote_uri;
    // If the Target specifies a custom fetch uri, use that.
    std::string uri_override = target.uri();
    if (uri_override.empty()) {
      ostree_remote_uri = ostree_server;
    } else {
      ostree_remote_uri = uri_override;
    }
    // addRemote overwrites any previous ostree remote that was set
    if (!OstreeManager::addRemote(repo.get(), ostree_remote_uri, keys)) {
      return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed,
                                      std::string("Error adding a default OSTree remote: ") + remote);
    }
  }

  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&builder, "{s@v}", "flags", g_variant_new_variant(g_variant_new_int32(0)));

  g_variant_builder_add(&builder, "{s@v}", "refs", g_variant_new_variant(g_variant_new_strv(commit_ids, 1)));

  if (!!headers && !(*headers).empty()) {
    GVariantBuilder hdr_builder;
    g_variant_builder_init(&hdr_builder, G_VARIANT_TYPE("a(ss)"));

    for (const auto &kv : *headers) {
      g_variant_builder_add(&hdr_builder, "(ss)", kv.first.c_str(), kv.second.c_str());
    }
    g_variant_builder_add(&builder, "{s@v}", "http-headers",
                          g_variant_new_variant(g_variant_builder_end(&hdr_builder)));
  }

  options = g_variant_builder_end(&builder);

  PullMetaStruct mt(target, token, g_cancellable_new(), std::move(progress_cb));
  progress.reset(ostree_async_progress_new_and_connect(aktualizr_progress_cb, &mt));
  if (ostree_repo_pull_with_options(repo.get(), alt_remote == nullptr ? remote : alt_remote, options, progress.get(),
                                    mt.cancellable.get(), &error) == 0) {
    LOG_ERROR << "Error while pulling image: " << error->code << " " << error->message;
    data::InstallationResult install_res(data::ResultCode::Numeric::kInstallFailed, error->message);
    g_error_free(error);
    g_variant_unref(options);
    return install_res;
  }
  ostree_async_progress_finish(progress.get());
  g_variant_unref(options);
  return data::InstallationResult(data::ResultCode::Numeric::kOk, "Pulling OSTree image was successful");
}

data::InstallationResult OstreeManager::install(const Uptane::Target &target) const {
  const char *opt_osname = nullptr;
  GCancellable *cancellable = nullptr;
  GError *error = nullptr;
  g_autofree char *revision = nullptr;

  if (!config.os.empty()) {
    opt_osname = config.os.c_str();
  }

  GObjectUniquePtr<OstreeSysroot> sysroot = OstreeManager::LoadSysroot(config.sysroot);
  GObjectUniquePtr<OstreeRepo> repo = LoadRepo(sysroot.get(), &error);

  if (error != nullptr) {
    LOG_ERROR << "could not get repo";
    g_error_free(error);
    return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, "could not get repo");
  }

  auto origin = StructGuard<GKeyFile>(
      ostree_sysroot_origin_new_from_refspec(sysroot.get(), target.sha256Hash().c_str()), g_key_file_free);
  if (ostree_repo_resolve_rev(repo.get(), target.sha256Hash().c_str(), FALSE, &revision, &error) == 0) {
    LOG_ERROR << error->message;
    data::InstallationResult install_res(data::ResultCode::Numeric::kInstallFailed, error->message);
    g_error_free(error);
    return install_res;
  }

  GObjectUniquePtr<OstreeDeployment> merge_deployment(ostree_sysroot_get_merge_deployment(sysroot.get(), opt_osname));
  if (merge_deployment == nullptr) {
    LOG_ERROR << "No merge deployment";
    return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, "No merge deployment");
  }

  if (ostree_sysroot_prepare_cleanup(sysroot.get(), cancellable, &error) == 0) {
    LOG_ERROR << error->message;
    data::InstallationResult install_res(data::ResultCode::Numeric::kInstallFailed, error->message);
    g_error_free(error);
    return install_res;
  }

  std::string args_content =
      std::string(ostree_bootconfig_parser_get(ostree_deployment_get_bootconfig(merge_deployment.get()), "options"));
  std::vector<std::string> args_vector;
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
  boost::split(args_vector, args_content, boost::is_any_of(" "));

  std::vector<const char *> kargs_strv_vector;
  kargs_strv_vector.reserve(args_vector.size() + 1);

  for (auto it = args_vector.begin(); it != args_vector.end(); ++it) {
    kargs_strv_vector.push_back((*it).c_str());
  }
  kargs_strv_vector[args_vector.size()] = nullptr;
  auto *kargs_strv = const_cast<char **>(&kargs_strv_vector[0]);

  OstreeDeployment *new_deployment_raw = nullptr;
  if (ostree_sysroot_deploy_tree(sysroot.get(), opt_osname, revision, origin.get(), merge_deployment.get(), kargs_strv,
                                 &new_deployment_raw, cancellable, &error) == 0) {
    LOG_ERROR << "ostree_sysroot_deploy_tree: " << error->message;
    data::InstallationResult install_res(data::ResultCode::Numeric::kInstallFailed, error->message);
    g_error_free(error);
    return install_res;
  }
  GObjectUniquePtr<OstreeDeployment> new_deployment = GObjectUniquePtr<OstreeDeployment>(new_deployment_raw);

  if (ostree_sysroot_simple_write_deployment(sysroot.get(), nullptr, new_deployment.get(), merge_deployment.get(),
                                             OSTREE_SYSROOT_SIMPLE_WRITE_DEPLOYMENT_FLAGS_NONE, cancellable,
                                             &error) == 0) {
    LOG_ERROR << "ostree_sysroot_simple_write_deployment:" << error->message;
    data::InstallationResult install_res(data::ResultCode::Numeric::kInstallFailed, error->message);
    g_error_free(error);
    return install_res;
  }

  // set reboot flag to be notified later
  if (bootloader_ != nullptr) {
    bootloader_->rebootFlagSet();
  }

  LOG_INFO << "Performing sync()";
  sync();
  return data::InstallationResult(data::ResultCode::Numeric::kNeedCompletion, "Application successful, need reboot");
}

void OstreeManager::completeInstall() const {
  LOG_INFO << "About to reboot the system in order to apply pending updates...";
  bootloader_->reboot();
}

data::InstallationResult OstreeManager::finalizeInstall(const Uptane::Target &target) {
  if (!bootloader_->rebootDetected()) {
    return data::InstallationResult(data::ResultCode::Numeric::kNeedCompletion,
                                    "Reboot is required for the pending update application");
  }

  LOG_INFO << "Checking installation of new OSTree sysroot";
  const std::string current_hash = getCurrentHash();

  data::InstallationResult install_result =
      data::InstallationResult(data::ResultCode::Numeric::kOk, "Successfully booted on new version");

  if (current_hash != target.sha256Hash()) {
    LOG_ERROR << "Expected to boot " << target.sha256Hash() << " but found " << current_hash
              << ". The system may have been rolled back.";
    install_result = data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, "Wrong version booted");
  }

  bootloader_->rebootFlagClear();
  return install_result;
}

void OstreeManager::updateNotify() { bootloader_->updateNotify(); }

OstreeManager::OstreeManager(const PackageConfig &pconfig, const BootloaderConfig &bconfig,
                             const std::shared_ptr<INvStorage> &storage, const std::shared_ptr<HttpInterface> &http,
                             Bootloader *bootloader)
    : PackageManagerInterface(pconfig, BootloaderConfig(), storage, http),
      bootloader_(bootloader == nullptr ? new Bootloader(bconfig, *storage) : bootloader) {
  GObjectUniquePtr<OstreeSysroot> sysroot_smart = OstreeManager::LoadSysroot(config.sysroot);
  if (sysroot_smart == nullptr) {
    throw std::runtime_error("Could not find OSTree sysroot at: " + config.sysroot.string());
  }

  // consider boot successful as soon as we started, missing internet connection or connection to Secondaries are not
  // proper reasons to roll back
  if (imageUpdated()) {
    bootloader_->setBootOK();
  }
}

OstreeManager::~OstreeManager() { bootloader_.reset(nullptr); }

bool OstreeManager::fetchTarget(const Uptane::Target &target, Uptane::Fetcher &fetcher, const KeyManager &keys,
                                const FetcherProgressCb &progress_cb, const api::FlowControlToken *token) {
  if (!target.IsOstree()) {
    // The case when the OSTree package manager is set as a package manager for aktualizr
    // while the target is aimed for a Secondary ECU that is configured with another/non-OSTree package manager
    return PackageManagerInterface::fetchTarget(target, fetcher, keys, progress_cb, token);
  }
  return OstreeManager::pull(config.sysroot, config.ostree_server, keys, target, token, progress_cb).success;
}

TargetStatus OstreeManager::verifyTarget(const Uptane::Target &target) const {
  if (!target.IsOstree()) {
    // The case when the OSTree package manager is set as a package manager for aktualizr
    // while the target is aimed for a Secondary ECU that is configured with another/non-OSTree package manager
    return PackageManagerInterface::verifyTarget(target);
  }
  return verifyTargetInternal(target);
}

TargetStatus OstreeManager::verifyTargetInternal(const Uptane::Target &target) const {
  const std::string refhash = target.sha256Hash();
  GError *error = nullptr;

  GObjectUniquePtr<OstreeSysroot> sysroot = OstreeManager::LoadSysroot(config.sysroot);
  GObjectUniquePtr<OstreeRepo> repo = LoadRepo(sysroot.get(), &error);
  if (error != nullptr) {
    LOG_ERROR << "Could not get OSTree repo";
    g_error_free(error);
    return TargetStatus::kNotFound;
  }

  GHashTable *ref_list = nullptr;
  if (ostree_repo_list_commit_objects_starting_with(repo.get(), refhash.c_str(), &ref_list, nullptr, &error) != 0) {
    guint length = g_hash_table_size(ref_list);
    g_hash_table_destroy(ref_list);  // OSTree creates the table with destroy notifiers, so no memory leaks expected
    // should never be greater than 1, but use >= for robustness
    if (length >= 1) {
      return TargetStatus::kGood;
    }
  }
  if (error != nullptr) {
    g_error_free(error);
    error = nullptr;
  }

  LOG_ERROR << "Could not find OSTree commit";
  return TargetStatus::kNotFound;
}

Json::Value OstreeManager::getInstalledPackages() const {
  std::string packages_str = Utils::readFile(config.packages_file);
  std::vector<std::string> package_lines;
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
  boost::split(package_lines, packages_str, boost::is_any_of("\n"));
  Json::Value packages(Json::arrayValue);
  for (auto it = package_lines.begin(); it != package_lines.end(); ++it) {
    if (it->empty()) {
      continue;
    }
    size_t pos = it->find(" ");
    if (pos == std::string::npos) {
      throw std::runtime_error("Wrong packages file format");
    }
    Json::Value package;
    package["name"] = it->substr(0, pos);
    package["version"] = it->substr(pos + 1);
    packages.append(package);
  }
  return packages;
}

std::string OstreeManager::getCurrentHash() const {
  OstreeDeployment *deployment = nullptr;
  GObjectUniquePtr<OstreeSysroot> sysroot_smart = OstreeManager::LoadSysroot(config.sysroot);
  if (config.booted == BootedType::kBooted) {
    deployment = ostree_sysroot_get_booted_deployment(sysroot_smart.get());
  } else {
    g_autoptr(GPtrArray) deployments = ostree_sysroot_get_deployments(sysroot_smart.get());
    if (deployments != nullptr && deployments->len > 0) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      deployment = static_cast<OstreeDeployment *>(deployments->pdata[0]);
    }
  }
  if (deployment == nullptr) {
    std::stringstream text;
    text << "Could not get " << config.booted << " deployment in " << config.sysroot.string();
    throw std::runtime_error(text.str());
  }
  return ostree_deployment_get_csum(deployment);
}

Uptane::Target OstreeManager::getCurrent() const {
  const std::string current_hash = getCurrentHash();
  boost::optional<Uptane::Target> current_version;
  // This may appear Primary-specific, but since Secondaries only know about
  // themselves, this actually works just fine for them, too.
  storage_->loadPrimaryInstalledVersions(&current_version, nullptr);

  if (!!current_version && current_version->sha256Hash() == current_hash) {
    return *current_version;
  }

  LOG_ERROR << "Current versions in storage and reported by OSTree do not match";

  // Look into installation log to find a possible candidate. Again, despite the
  // name, this will work for Secondaries as well.
  std::vector<Uptane::Target> installed_versions;
  storage_->loadPrimaryInstallationLog(&installed_versions, false);

  // Version should be in installed versions. It's possible that multiple
  // targets could have the same sha256Hash. In this case the safest assumption
  // is that the most recent (the reverse of the vector) target is what we
  // should return.
  std::vector<Uptane::Target>::reverse_iterator it;
  for (it = installed_versions.rbegin(); it != installed_versions.rend(); it++) {
    if (it->sha256Hash() == current_hash) {
      return *it;
    }
  }
  // We haven't found a matching target. This can occur when a device is
  // freshly manufactured and the factory image is in a delegated target.
  // Aktualizr will have had no reason to fetch the relevant delegation, and it
  // doesn't know where in the delegation tree on the server it might be.
  // See https://github.com/uptane/aktualizr/issues/1 for more details. In this
  // case attempt to construct an approximate Uptane target. By getting the
  // hash correct the server has a chance to figure out what is running on the
  // device.
  Uptane::EcuMap ecus;
  std::vector<Hash> hashes{Hash(Hash::Type::kSha256, current_hash)};
  return {"unknown", ecus, hashes, 0, "OSTREE"};
}

// used for bootloader rollback
bool OstreeManager::imageUpdated() {
  GObjectUniquePtr<OstreeSysroot> sysroot_smart = OstreeManager::LoadSysroot(config.sysroot);

  // image updated if no pending deployment in the list of deployments
  GPtrArray *deployments = ostree_sysroot_get_deployments(sysroot_smart.get());

  OstreeDeployment *pending_deployment = nullptr;
  ostree_sysroot_query_deployments_for(sysroot_smart.get(), nullptr, &pending_deployment, nullptr);

  bool pending_found = false;
  for (guint i = 0; i < deployments->len; i++) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (deployments->pdata[i] == pending_deployment) {
      pending_found = true;
      break;
    }
  }

  g_ptr_array_unref(deployments);
  return !pending_found;
}

GObjectUniquePtr<OstreeDeployment> OstreeManager::getStagedDeployment() const {
  GObjectUniquePtr<OstreeSysroot> sysroot_smart = OstreeManager::LoadSysroot(config.sysroot);

  GPtrArray *deployments = nullptr;
  OstreeDeployment *res = nullptr;

  deployments = ostree_sysroot_get_deployments(sysroot_smart.get());

  if (deployments->len > 0) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto *d = static_cast<OstreeDeployment *>(deployments->pdata[0]);
    auto *d2 = static_cast<OstreeDeployment *>(g_object_ref(d));
    res = d2;
  }

  g_ptr_array_unref(deployments);
  return GObjectUniquePtr<OstreeDeployment>(res);
}

GObjectUniquePtr<OstreeSysroot> OstreeManager::LoadSysroot(const boost::filesystem::path &path) {
  GObjectUniquePtr<OstreeSysroot> sysroot = nullptr;

  if (!path.empty()) {
    GFile *fl = g_file_new_for_path(path.c_str());
    sysroot.reset(ostree_sysroot_new(fl));
    g_object_unref(fl);
  } else {
    sysroot.reset(ostree_sysroot_new_default());
  }
  GError *error = nullptr;
  if (ostree_sysroot_load(sysroot.get(), nullptr, &error) == 0) {
    const std::string msg = error->message;
    if (error != nullptr) {
      g_error_free(error);
    }
    throw std::runtime_error("could not load sysroot at " + path.string() + ": " + msg);
  }
  return sysroot;
}

GObjectUniquePtr<OstreeRepo> OstreeManager::LoadRepo(OstreeSysroot *sysroot, GError **error) {
  OstreeRepo *repo = nullptr;

  if (ostree_sysroot_get_repo(sysroot, &repo, nullptr, error) == 0) {
    return nullptr;
  }

  return GObjectUniquePtr<OstreeRepo>(repo);
}

bool OstreeManager::addRemote(OstreeRepo *repo, const std::string &url, const KeyManager &keys) {
  GCancellable *cancellable = nullptr;
  GError *error = nullptr;
  GVariantBuilder b;
  GVariant *options;

  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&b, "{s@v}", "gpg-verify", g_variant_new_variant(g_variant_new_boolean(FALSE)));

  std::string cert_file = keys.getCertFile();
  std::string pkey_file = keys.getPkeyFile();
  std::string ca_file = keys.getCaFile();
  if (!cert_file.empty() && !pkey_file.empty() && !ca_file.empty()) {
    g_variant_builder_add(&b, "{s@v}", "tls-client-cert-path",
                          g_variant_new_variant(g_variant_new_string(cert_file.c_str())));
    g_variant_builder_add(&b, "{s@v}", "tls-client-key-path",
                          g_variant_new_variant(g_variant_new_string(pkey_file.c_str())));
    g_variant_builder_add(&b, "{s@v}", "tls-ca-path", g_variant_new_variant(g_variant_new_string(ca_file.c_str())));
  }
  options = g_variant_builder_end(&b);

  if (ostree_repo_remote_change(repo, nullptr, OSTREE_REPO_REMOTE_CHANGE_DELETE_IF_EXISTS, remote, url.c_str(), options,
                                cancellable, &error) == 0) {
    LOG_ERROR << "Error of adding remote: " << error->message;
    g_error_free(error);
    g_variant_unref(options);
    return false;
  }
  if (ostree_repo_remote_change(repo, nullptr, OSTREE_REPO_REMOTE_CHANGE_ADD_IF_NOT_EXISTS, remote, url.c_str(),
                                options, cancellable, &error) == 0) {
    LOG_ERROR << "Error of adding remote: " << error->message;
    g_error_free(error);
    g_variant_unref(options);
    return false;
  }
  g_variant_unref(options);
  return true;
}
