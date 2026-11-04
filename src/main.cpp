#include "internal.hpp"
#include "cli_control.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <syrnck/git.hpp>
#include <syrnck/git_net.hpp>
#include <syrnck/net.hpp>
#include <syrnck/i18n.hpp>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
namespace {
using namespace syrnck;
class Args {
public:
  std::vector<std::string> values;
  std::optional<std::string> option(std::string_view name, std::string_view alias = {}) {
    std::optional<std::string> result;
    for (std::size_t i = 0; i < values.size();) {
      if (values[i] == "--")
        break;
      if (values[i] == name || (!alias.empty() && values[i] == alias)) {
        require(!result && i + 1 < values.size(), "missing or duplicate option value");
        result = values[i + 1];
        values.erase(values.begin() + static_cast<std::ptrdiff_t>(i),
                     values.begin() + static_cast<std::ptrdiff_t>(i + 2));
      } else
        ++i;
    }
    return result;
  }
  bool flag(std::string_view name) {
    bool found = false;
    for (auto it = values.begin(); it != values.end();) {
      if (*it == "--")
        break;
      if (*it == name) {
        require(!found, "duplicate flag");
        found = true;
        it = values.erase(it);
      } else
        ++it;
    }
    return found;
  }
  void count(std::size_t n) {
    if (auto it = std::find(values.begin(), values.end(), "--"); it != values.end())
      values.erase(it);
    require(values.size() == n, "unexpected or missing arguments; use --help");
  }
  std::string pop() {
    require(!values.empty(), "missing command; use --help");
    auto value = values.front();
    values.erase(values.begin());
    return value;
  }
};
std::optional<std::string> environment(const char *name) {
  auto value = std::getenv(name);
  if (!value)
    return {};
  return std::string(value);
}
std::optional<Key> read_key() {
  auto value = environment("SYRNCK_KEY");
  if (!value)
    return {};
  auto data = unhex(*value);
  require(data.size() == 32, "SYRNCK_KEY must encode exactly 32 bytes");
  Key key{};
  std::copy(data.begin(), data.end(), key.begin());
  return key;
}
void print(const Json &j) {
  std::cout << j.dump(2) << '\n';
}
std::uint64_t integer(const std::string &s) {
  require(!s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }),
          "invalid unsigned integer");
  return std::stoull(s);
}
IngestOptions ingestion_options(Args &args, const Store &store) {
  auto options = store.settings().ingest_options();
  if (auto workers = args.option("--workers")) {
    auto n = integer(*workers);
    require(n >= 1 && n <= 64, "ingest workers must be in 1..64");
    options.workers = static_cast<std::size_t>(n);
  }
  if (auto budget = args.option("--buffer-mib")) {
    auto n = integer(*budget);
    require(n >= 1 && n <= 1024, "buffer budget must be in 1..1024 MiB");
    options.max_in_flight_bytes = static_cast<std::size_t>(n * 1024 * 1024);
  }
  return options;
}
net::TransferOptions transfer_options(Args &args) {
  net::TransferOptions limits;
  if (auto compression = args.option("--compression")) {
    require(*compression == "auto" || *compression == "off" || *compression == "zstd",
            "compression must be auto, off or zstd");
    limits.compression = *compression == "off"    ? net::ObjectCompression::disabled
                         : *compression == "zstd" ? net::ObjectCompression::zstd
                                                  : net::ObjectCompression::automatic;
  }
  limits.require_upload_lease = args.flag("--require-lease");
  limits.require_read_lease = args.flag("--require-read-lease");
  if (auto requests = args.option("--requests")) {
    auto count = integer(*requests);
    require(count >= 1 && count <= 32, "transfer requests must be in 1..32");
    limits.max_requests = static_cast<std::size_t>(count);
  }
  if (auto budget = args.option("--transfer-buffer-mib")) {
    auto size = integer(*budget);
    require(size >= 17 && size <= 1024, "transfer buffer must be in 17..1024 MiB");
    limits.max_in_flight_bytes = static_cast<std::size_t>(size * 1024 * 1024);
  }
  if (auto retries = args.option("--retries")) {
    auto count = integer(*retries);
    require(count <= 8, "transfer retries must be in 0..8");
    limits.max_retries = static_cast<std::size_t>(count);
  }
  auto rate = [&](std::string_view option) {
    auto value = args.option(option);
    auto kib = value ? integer(*value) : 0;
    require(kib <= (1ULL << 30), "transfer rate must be in 0..1073741824 KiB/s");
    return kib * 1024;
  };
  limits.upload_bytes_per_second = rate("--upload-kib-s");
  limits.download_bytes_per_second = rate("--download-kib-s");
  limits.validate();
  return limits;
}
void help(Language language) {
  if (language == Language::french) {
    std::cout << R"(Syrnck 0.1.0 — Stockage de blocs C++20, assets Git et révisions natives
Usage : syrnck [--lang en|fr] [--store CHEMIN] [--token JETON] [--ca PEM]
               [--cache-mib 64] [--index-cache-mib 4] COMMANDE
  init [--encrypted]                 Créer un stockage de blocs
  keygen                             Afficher une clé aléatoire de 256 bits
  put FICHIER [--fixed-size OCTETS] [--workers 1..64] [--buffer-mib 32]
                                     Importer et conserver un fichier complet
  get MANIFESTE NOUVEAU_FICHIER       Reconstruire sans écraser de fichier
  inspect manifest|revision ID       Inspecter les métadonnées vérifiées
  diff ANCIEN_MANIFESTE NOUVEAU_MANIFESTE
                                     Comparer les blocs réutilisables
  refs                               Lister les racines de rétention
  pin NOM TYPE ID | unpin NOM         Gérer les racines de rétention
  gc [--delete]                       Nettoyer les objets (simulation par défaut)
  pack [--compact] [--max-mib 512] [--max-objects 65536]
                                     Regrouper les objets ; plusieurs packs exigent le format v3
  fsck                               Vérifier les objets et les hashes des fichiers
  config show | reset                Afficher ou réinitialiser les réglages du dépôt
  config set [--workers N] [--buffer-mib N] [--pack-max-mib N] [--pack-max-objects N]
  native commit [-m MESSAGE] [--branch main] [--directory .]
                [--workers N] [--buffer-mib N]
  native branch NOM RÉVISION
  native log [--branch main] [--limit 50]
  native export RÉVISION NOUVEAU_DOSSIER
  native detach-history -m MESSAGE [--branch main]
  git install [DOSSIER] [--encrypted]
  git catalog COMMIT [--directory CHEMIN]   Inspecter les pointeurs du commit
  git prepare-assets COMMIT [--directory CHEMIN]   Vérifier et conserver ses assets
  git push-assets URL COMMIT | git pull-assets URL COMMIT [--requests N]
  git configure-remote NOM URL | git install-hook [--directory CHEMIN]
  git configure-checkout local|download|skip [--remote NOM] [--directory CHEMIN]
  git clean [--workers N] [--buffer-mib N] | git smudge
  git filter-process [--checkout-policy]   Protocole Git v2, plusieurs fichiers par processus
  serve [--listen 127.0.0.1:7437]      HTTP local ; publier derrière un proxy HTTPS
        [--lease-seconds 3600] [--read-lease-seconds 3600] [--require-lease]
  leases list | release ID | prune [--read]   Gérer les baux du store serveur
  agent push URL RÉF_LOCALE RÉF_DISTANTE
             [--require-lease]      Exiger la protection du GC côté serveur
  agent pull URL RÉF_DISTANTE RÉF_LOCALE [--require-read-lease]
  agent resume URL SESSION           Reprendre une session interrompue
             [--requests 1..32] [--transfer-buffer-mib 64] (push, pull ou resume)
             [--retries 0..8] [--upload-kib-s 0] [--download-kib-s 0]
  agent sessions                     Lister les sessions persistantes locales
  agent forget SESSION              Supprimer le journal et libérer sa protection GC
  agent prune [--delete]             Prévisualiser ou purger les reçus terminés
Compression : --compression auto|off|zstd ; serveur : --no-compression.
Transferts : 3 relances par requête ; débits partagés en Kio/s (0 = illimité).
Langue : anglais par défaut ; --lang en|fr remplace SYRNCK_LANG.
Progression : --progress ou --progress-json sur stderr pour put, get, pack, gc,
              fsck, native commit/export, git clean/smudge/filter-process et agent push/pull/resume/prune.
              Ctrl+C annule (code 130) avant le point de publication.
Secrets : SYRNCK_KEY (64 caractères hexadécimaux), SYRNCK_TOKEN (32 caractères minimum).
Checkout : local par défaut ; SYRNCK_SKIP_SMUDGE=1 conserve les pointeurs sans store.
)";
    return;
  }
  std::cout << R"(Syrnck 0.1.0 — C++20 block storage, Git assets and native revisions
Usage: syrnck [--lang en|fr] [--store PATH] [--token TOKEN] [--ca PEM] [--cache-mib 64]
              [--index-cache-mib 4] COMMAND
  init [--encrypted]                 Create a new block store
  keygen                             Print a random 256-bit key
  put FILE [--fixed-size BYTES] [--workers 1..64] [--buffer-mib 32]
                                     Ingest and retain a complete file
  get MANIFEST NEW_FILE              Reconstruct without overwriting
  inspect manifest|revision ID       Inspect verified metadata
  diff OLD_MANIFEST NEW_MANIFEST      Compare reusable file blocks
  refs                              List retention roots
  pin NAME KIND ID | unpin NAME       Manage retention roots
  gc [--delete]                      Mark-and-sweep (dry run by default)
  pack [--compact] [--max-mib 512] [--max-objects 65536]
                                    Pack loose objects; multiple packs require storage v3
  fsck                              Verify all objects and full file hashes
  config show | reset               Show or reset repository settings
  config set [--workers N] [--buffer-mib N] [--pack-max-mib N] [--pack-max-objects N]
  native commit [-m MESSAGE] [--branch main] [--directory .]
                [--workers N] [--buffer-mib N]
  native branch NAME REVISION
  native log [--branch main] [--limit 50]
  native export REVISION NEW_DIRECTORY
  native detach-history -m MESSAGE [--branch main]
  git install [DIRECTORY] [--encrypted]
  git catalog COMMIT [--directory PATH]   Inspect committed asset pointers
  git prepare-assets COMMIT [--directory PATH]   Verify and retain its assets
  git push-assets URL COMMIT | git pull-assets URL COMMIT [--requests N]
  git configure-remote NAME URL | git install-hook [--directory PATH]
  git configure-checkout local|download|skip [--remote NAME] [--directory PATH]
  git clean [--workers N] [--buffer-mib N] | git smudge
  git filter-process [--checkout-policy]   Git protocol v2, multiple files per process
  serve [--listen 127.0.0.1:7437]      Loopback HTTP; publish behind HTTPS
        [--lease-seconds 3600] [--read-lease-seconds 3600] [--require-lease]
  leases list | release ID | prune [--read]   Manage server store leases
  agent push URL LOCAL_REF REMOTE_REF
             [--require-lease]      Require server GC protection
  agent pull URL REMOTE_REF LOCAL_REF [--require-read-lease]
  agent resume URL SESSION           Resume an interrupted session
             [--requests 1..32] [--transfer-buffer-mib 64] (push, pull or resume)
             [--retries 0..8] [--upload-kib-s 0] [--download-kib-s 0]
  agent sessions                     List persistent local sessions
  agent forget SESSION              Discard the journal and release its GC hold
  agent prune [--delete]             Preview or prune completed receipts
Compression: --compression auto|off|zstd; server: --no-compression.
Transfers: 3 retries per request; shared rates in KiB/s (0 = unlimited).
Language: English by default; --lang en|fr overrides SYRNCK_LANG.
Progress: --progress or --progress-json on stderr for put, get, pack, gc,
              fsck, native commit/export, git clean/smudge/filter-process and agent push/pull/resume/prune.
          Ctrl+C cancels (exit 130) before the publication commit point.
Secrets: SYRNCK_KEY (64 hex characters), SYRNCK_TOKEN (at least 32 characters).
Checkout: local by default; SYRNCK_SKIP_SMUDGE=1 preserves pointers without a store.
)";
}
int run(Args args, Language language, std::unique_ptr<cli::OperationControl> &control) {
  if (args.flag("--help") || args.flag("-h") || args.values.empty()) {
    help(language);
    return 0;
  }
  if (args.flag("--version") || args.flag("-V")) {
    std::cout << "syrnck 0.1.0 (C++20)\n";
    return 0;
  }
  auto root_option = args.option("--store");
  auto root = path_from_utf8(root_option.value_or(".syrnck"));
  StoreOptions store_options;
  if (auto cache_mib = args.option("--cache-mib")) {
    auto size = integer(*cache_mib);
    require(size <= 1024, "cache budget must be in 0..1024 MiB");
    store_options.decoded_cache_bytes = static_cast<std::size_t>(size * 1024 * 1024);
    if (size == 0)
      store_options.verified_cache_entries = 0;
  }
  if (auto index_mib = args.option("--index-cache-mib")) {
    auto size = integer(*index_mib);
    require(size <= 1024, "index cache budget must be in 0..1024 MiB");
    store_options.pack_index_cache_bytes = static_cast<std::size_t>(size * 1024 * 1024);
  }
  auto token = args.option("--token");
  if (!token)
    token = environment("SYRNCK_TOKEN");
  auto ca = args.option("--ca");
  std::optional<fs::path> ca_path;
  if (ca)
    ca_path = path_from_utf8(*ca);
  auto command = args.pop();
  if (command == "keygen") {
    args.count(0);
    std::cout << hex(random_bytes(32)) << '\n';
    return 0;
  }
  std::string sub;
  if (command == "git" || command == "native" || command == "agent" || command == "config" ||
      command == "leases")
    sub = args.pop();
  if (command == "git" && (sub == "smudge" || sub == "filter-process")) {
    auto directory = path_from_utf8(args.option("--directory").value_or("."));
    auto configured = args.flag("--checkout-policy");
    auto skip = environment("SYRNCK_SKIP_SMUDGE").value_or("0");
    require(skip == "0" || skip == "1", "SYRNCK_SKIP_SMUDGE must be 0 or 1");
    git::CheckoutPolicy policy;
    if (skip == "1")
      policy.mode = git::CheckoutMode::skip;
    else if (configured)
      policy = git::checkout_policy(directory);
    Args ingest_arguments;
    if (sub == "filter-process") {
      for (const auto *name : {"--workers", "--buffer-mib"})
        if (auto value = args.option(name))
          ingest_arguments.values.insert(ingest_arguments.values.end(), {name, *value});
    }
    auto limits = transfer_options(args);
    require(!limits.require_upload_lease, "upload lease requirement is only available for push");
    auto text = args.flag("--progress"), json = args.flag("--progress-json");
    require(!(text && json), "choose one progress output mode");
    args.count(0);
    control = std::make_unique<cli::OperationControl>(language, json   ? cli::ProgressMode::json
                                                                : text ? cli::ProgressMode::text
                                                                       : cli::ProgressMode::none);
    OperationOptions operation{control->token(), [&](const ProgressEvent &event) { control->report(event); }};
    std::unique_ptr<Store> store;
    auto storage = [&]() -> const Store & {
      if (!store) {
        if (configured && !root_option)
          root = git::common_store(directory);
        store = std::make_unique<Store>(root, read_key(), false, store_options);
      }
      return *store;
    };
    std::unique_ptr<net::Remote> remote;
    git::AssetResolver resolve;
    if (policy.mode == git::CheckoutMode::download) {
      limits.require_read_lease = true;
      resolve = [&](const git::Pointer &pointer) {
        if (!remote) {
          require(token.has_value(), "set SYRNCK_TOKEN");
          remote = std::make_unique<net::Remote>(git::remote_url(directory, policy.remote), *token, ca_path);
        }
        (void)git::hydrate_asset(storage(), pointer, *remote, operation, limits);
      };
    }
    auto handler = [&](std::string_view action, std::string_view, std::istream &input, std::ostream &output) {
      if (action == "clean") {
        auto arguments = ingest_arguments;
        auto options = ingestion_options(arguments, storage());
        options.stop = operation.stop;
        options.progress = operation.progress;
        git::clean(storage(), input, output, options);
      } else if (policy.mode == git::CheckoutMode::skip) {
        git::skip_smudge(input, output, operation);
      } else {
        git::smudge(storage(), input, output, operation, resolve);
      }
    };
    if (sub == "filter-process")
      git::filter_process(std::cin, std::cout, handler, operation, [&](std::string_view error) {
        std::cerr << localize("Error: ", language) << localize(error, language) << '\n';
      });
    else
      handler("smudge", {}, std::cin, std::cout);
    require(std::cout.good(), "output write failed");
    return 0;
  }
  auto key = read_key();
  if (command == "init") {
    auto encrypted = args.flag("--encrypted");
    args.count(0);
    require(encrypted == key.has_value(), "use --encrypted with SYRNCK_KEY, or omit both");
    Store store(root, key, true, store_options);
    std::cout << localize("Initialized ", language) << utf8(store.root())
              << localize(" (encrypted: ", language)
              << localize(store.encrypted() ? "true" : "false", language) << ")\n";
    return 0;
  }
  if (command == "git" && sub == "install") {
    auto encrypted = args.flag("--encrypted");
    require(encrypted == key.has_value(), "use --encrypted with SYRNCK_KEY, or omit both");
    auto directory = args.values.empty() ? fs::path(".") : path_from_utf8(args.pop());
    args.count(0);
    auto common = git::common_store(directory);
    Store store(common, key, !fs::exists(common / "config.json"), store_options);
    git::install(directory, executable_path(), store.root());
    std::cout << localize("Installed repository-local Git filters. Store: ", language) << utf8(store.root())
              << localize("\nAssign filter=syrnck -text to assets in .gitattributes.\n", language);
    return 0;
  }
  if (command == "git" && sub == "catalog") {
    auto directory = path_from_utf8(args.option("--directory").value_or("."));
    auto text = args.flag("--progress"), json = args.flag("--progress-json");
    require(!(text && json), "choose one progress output mode");
    args.count(1);
    control = std::make_unique<cli::OperationControl>(language, json   ? cli::ProgressMode::json
                                                                : text ? cli::ProgressMode::text
                                                                       : cli::ProgressMode::none);
    print(git::catalog(directory, args.values[0],
                       {control->token(), [&](const ProgressEvent &event) { control->report(event); }})
              .json());
    return 0;
  }
  if (command == "git" &&
      (sub == "configure-remote" || sub == "configure-checkout" || sub == "install-hook")) {
    auto directory = path_from_utf8(args.option("--directory").value_or("."));
    if (sub == "configure-remote") {
      args.count(2);
      const net::Remote validate(args.values[1], std::string(32, '_'));
      (void)validate;
      git::configure_remote(directory, args.values[0], args.values[1]);
      print(Json{{"remote", args.values[0]}, {"configured", true}});
    } else if (sub == "configure-checkout") {
      auto remote = args.option("--remote").value_or("");
      args.count(1);
      auto mode = args.values[0];
      require(mode == "local" || mode == "download" || mode == "skip", "invalid Git checkout mode");
      git::CheckoutPolicy policy{mode == "download" ? git::CheckoutMode::download
                                 : mode == "skip"   ? git::CheckoutMode::skip
                                                    : git::CheckoutMode::local,
                                 remote};
      auto location = root_option ? root : git::common_store(directory);
      require(fs::is_regular_file(location / "config.json"),
              "initialize Git filters before configuring checkout");
      git::install(directory, executable_path(), fs::absolute(location));
      git::configure_checkout(directory, policy);
      print(Json{{"mode", mode}, {"remote", remote}});
    } else {
      args.count(0);
      auto location = root_option ? root : git::common_store(directory);
      require(fs::is_regular_file(location / "config.json"),
              "initialize Git filters before installing the hook");
      git::install_pre_push(directory, executable_path(), location);
      print(Json{{"installed", true}});
    }
    return 0;
  }
  auto git_directory =
      command == "git" ? path_from_utf8(args.option("--directory").value_or(".")) : fs::path{};
  if (command == "git" &&
      (sub == "prepare-assets" || sub == "push-assets" || sub == "pull-assets" || sub == "pre-push") &&
      !root_option)
    root = git::common_store(git_directory);
  Store store(root, key, false, store_options);
  auto text_progress = args.flag("--progress"), json_progress = args.flag("--progress-json");
  require(!(text_progress && json_progress), "choose one progress output mode");
  bool cancellable =
      command == "put" || command == "get" || command == "fsck" || command == "pack" || command == "gc" ||
      (command == "native" && (sub == "commit" || sub == "export")) ||
      (command == "agent" && (sub == "push" || sub == "pull" || sub == "resume" || sub == "prune")) ||
      (command == "git" && (sub == "clean" || sub == "smudge" || sub == "prepare-assets" ||
                            sub == "push-assets" || sub == "pull-assets" || sub == "pre-push"));
  require(cancellable || (!text_progress && !json_progress), "progress is unavailable for this command");
  if (cancellable)
    control = std::make_unique<cli::OperationControl>(language, json_progress   ? cli::ProgressMode::json
                                                                : text_progress ? cli::ProgressMode::text
                                                                                : cli::ProgressMode::none);
  auto observe = [&](const ProgressEvent &event) { control->report(event); };
  if (command == "config") {
    if (sub == "show") {
      args.count(0);
      print(store.settings().json());
    } else if (sub == "reset") {
      args.count(0);
      store.set_settings({});
      print(store.settings().json());
    } else if (sub == "set") {
      auto workers = args.option("--workers"), buffer = args.option("--buffer-mib"),
           max_mib = args.option("--pack-max-mib"), max_objects = args.option("--pack-max-objects");
      args.count(0);
      require(workers || buffer || max_mib || max_objects, "no repository settings supplied");
      auto settings = store.settings();
      if (workers) {
        auto n = integer(*workers);
        require(n >= 1 && n <= 64, "ingest workers must be in 1..64");
        settings.ingest_workers = static_cast<std::size_t>(n);
      }
      if (buffer) {
        auto n = integer(*buffer);
        require(n >= 1 && n <= 1024, "buffer budget must be in 1..1024 MiB");
        settings.ingest_buffer_bytes = static_cast<std::size_t>(n * 1024 * 1024);
      }
      if (max_mib) {
        auto n = integer(*max_mib);
        require(n >= 18 && n <= 4096, "pack size limit must be in 18..4096 MiB");
        settings.pack_max_bytes = n * 1024 * 1024;
      }
      if (max_objects) {
        auto n = integer(*max_objects);
        require(n >= 1 && n <= 1048576, "pack object limit must be in 1..1048576");
        settings.pack_max_objects = static_cast<std::size_t>(n);
      }
      store.set_settings(settings);
      print(settings.json());
    } else
      throw Error("unknown config command");
  } else if (command == "put") {
    auto size = args.option("--fixed-size");
    auto options = ingestion_options(args, store);
    args.count(1);
    auto source = path_from_utf8(args.values[0]);
    std::ifstream in(source, std::ios::binary);
    require(in.is_open(), "cannot open source file");
    std::error_code size_error;
    auto source_size = fs::file_size(source, size_error);
    options.stop = control->token();
    options.progress = [&](const ProgressEvent &event) {
      auto contextual = event;
      contextual.path = args.values[0];
      if (!size_error)
        contextual.total_bytes = source_size;
      control->report(contextual);
    };
    Chunking chunking;
    if (size) {
      auto n = integer(*size);
      require(n <= UINT32_MAX, "chunk size overflow");
      chunking = Chunking::fixed_size(static_cast<std::uint32_t>(n));
    }
    auto [id, stats] = store.ingest(in, chunking, options);
    store.set_ref("files/" + id, {Kind::manifest, id}, options.stop);
    print(Json{{"manifest", id}, {"stats", stats.json()}});
  } else if (command == "get") {
    args.count(2);
    store.restore_file(args.values[0], path_from_utf8(args.values[1]),
                       {control->token(), [&](const ProgressEvent &event) {
                          auto contextual = event;
                          contextual.path = args.values[1];
                          control->report(contextual);
                        }});
  } else if (command == "inspect") {
    args.count(2);
    print(store.metadata(parse_kind(args.values[0]), args.values[1]));
  } else if (command == "diff") {
    args.count(2);
    auto old = store.metadata(Kind::manifest, args.values[0]),
         current = store.metadata(Kind::manifest, args.values[1]);
    std::set<std::string> ids;
    for (const auto &b : old["blocks"])
      ids.insert(b["id"].get<std::string>());
    std::uint64_t reused = 0;
    for (const auto &b : current["blocks"])
      if (ids.contains(b["id"].get<std::string>()))
        reused += b["size"].get<std::uint64_t>();
    auto size = current["size"].get<std::uint64_t>();
    print(Json{{"size", size}, {"reusable_bytes", reused}, {"changed_bytes", size - reused}});
  } else if (command == "refs") {
    args.count(0);
    Json j = Json::object();
    for (const auto &[name, value] : store.refs())
      j[name] = key_json(value);
    print(j);
  } else if (command == "pin") {
    args.count(3);
    store.set_ref(args.values[0], {parse_kind(args.values[1]), args.values[2]});
  } else if (command == "unpin") {
    args.count(1);
    store.remove_ref(args.values[0]);
  } else if (command == "gc") {
    auto remove = args.flag("--delete");
    args.count(0);
    print(store.gc(remove, {control->token(), observe}));
  } else if (command == "pack") {
    auto options = store.settings().pack_options();
    options.compact = args.flag("--compact");
    if (auto size = args.option("--max-mib")) {
      auto n = integer(*size);
      require(n >= 18 && n <= 4096, "pack size limit must be in 18..4096 MiB");
      options.max_pack_bytes = n * 1024 * 1024;
    }
    if (auto count = args.option("--max-objects")) {
      auto n = integer(*count);
      require(n >= 1 && n <= 1048576, "pack object limit must be in 1..1048576");
      options.max_objects_per_pack = static_cast<std::size_t>(n);
    }
    args.count(0);
    print(store.pack(options, {control->token(), observe}));
  } else if (command == "fsck") {
    args.count(0);
    auto checked = store.fsck({control->token(), observe});
    std::cout << localize("Verified ", language) << checked
              << localize(" objects and every file hash\n", language);
  } else if (command == "native") {
    if (sub == "commit") {
      auto options = ingestion_options(args, store);
      options.stop = control->token();
      options.progress = observe;
      auto directory = path_from_utf8(args.option("--directory").value_or("."));
      auto branch = args.option("--branch").value_or("main");
      auto message = args.option("--message", "-m");
      args.count(0);
      require(message.has_value(), "--message is required");
      std::cout << store.commit_directory(directory, branch, *message, options) << '\n';
    } else if (sub == "branch") {
      args.count(2);
      auto name = "heads/" + args.values[0];
      require(!store.refs().contains(name), "branch already exists");
      store.set_ref(name, {Kind::revision, args.values[1]});
    } else if (sub == "log") {
      auto branch = args.option("--branch").value_or("main");
      auto limit = integer(args.option("--limit").value_or("50"));
      args.count(0);
      auto refs = store.refs();
      auto name = "heads/" + branch;
      require(refs.contains(name), "unknown branch");
      auto current = refs.at(name);
      std::set<std::string> seen;
      for (std::uint64_t i = 0; i < limit; ++i) {
        require(current.kind == Kind::revision && seen.insert(current.id).second, "invalid history");
        auto j = store.metadata(Kind::revision, current.id);
        std::cout << current.id << ' ' << j["timestamp"] << ' ' << j["message"].get<std::string>() << '\n';
        if (j["parents"].empty())
          break;
        current.id = j["parents"][0].get<std::string>();
      }
    } else if (sub == "export") {
      args.count(2);
      store.export_revision(args.values[0], path_from_utf8(args.values[1]), {control->token(), observe});
    } else if (sub == "detach-history") {
      auto branch = args.option("--branch").value_or("main");
      auto message = args.option("--message", "-m");
      args.count(0);
      require(message.has_value(), "--message is required");
      std::cout << store.detach_history(branch, *message) << '\n';
    } else
      throw Error("unknown native command");
  } else if (command == "git") {
    if (sub == "pre-push") {
      auto limits = transfer_options(args);
      require(!limits.require_read_lease, "read lease requirement is only available for pull");
      limits.require_upload_lease = true;
      args.count(2);
      auto commits = git::push_commits(git_directory, std::cin, {control->token(), observe});
      Json summary{{"commits_scanned", std::uint64_t{0}},
                   {"catalogs_published", std::uint64_t{0}},
                   {"assets", std::uint64_t{0}},
                   {"transferred_objects", std::uint64_t{0}},
                   {"transferred_bytes", std::uint64_t{0}},
                   {"object_upload_bytes", std::uint64_t{0}},
                   {"object_download_bytes", std::uint64_t{0}},
                   {"object_retries", std::uint64_t{0}}};
      auto add = [&](std::string_view field, std::uint64_t amount) {
        auto &value = summary[std::string(field)];
        require(amount <= std::numeric_limits<std::uint64_t>::max() - value.get<std::uint64_t>(),
                "Git transfer counter overflow");
        value = value.get<std::uint64_t>() + amount;
      };
      std::unique_ptr<net::Remote> remote;
      for (const auto &commit : commits) {
        auto catalog = git::catalog(git_directory, commit, {control->token(), observe});
        add("commits_scanned", 1);
        if (catalog.assets.empty())
          continue;
        if (!remote) {
          require(token.has_value(), "set SYRNCK_TOKEN");
          remote =
              std::make_unique<net::Remote>(git::remote_url(git_directory, args.values[0]), *token, ca_path);
        }
        auto result = git::push_assets(store, catalog, *remote, {control->token(), observe}, limits);
        add("catalogs_published", 1);
        add("assets", catalog.assets.size());
        for (const auto *field : {"transferred_objects", "transferred_bytes", "object_upload_bytes",
                                  "object_download_bytes", "object_retries"})
          add(field, result["transfer"][field].get<std::uint64_t>());
      }
      print(summary);
    } else if (sub == "push-assets" || sub == "pull-assets") {
      auto limits = transfer_options(args);
      args.count(2);
      auto catalog = git::catalog(git_directory, args.values[1], {control->token(), observe});
      require(token.has_value(), "set SYRNCK_TOKEN");
      net::Remote remote(args.values[0], *token, ca_path);
      print(sub == "push-assets"
                ? git::push_assets(store, catalog, remote, {control->token(), observe}, limits)
                : git::pull_assets(store, catalog, remote, {control->token(), observe}, limits));
    } else if (sub == "prepare-assets") {
      args.count(1);
      auto catalog = git::catalog(git_directory, args.values[0], {control->token(), observe});
      auto bundle = git::prepare_bundle(store, catalog, {control->token(), observe});
      print(
          Json{{"commit", catalog.commit}, {"assets", catalog.assets.size()}, {"bundle", key_json(bundle)}});
    } else if (sub == "clean") {
      auto options = ingestion_options(args, store);
      options.stop = control->token();
      options.progress = observe;
      args.count(0);
      git::clean(store, std::cin, std::cout, options);
    } else
      throw Error("unknown Git command");
  } else if (command == "leases") {
    auto reads = args.flag("--read");
    if (sub == "list") {
      args.count(0);
      auto result = Json::array();
      if (reads) {
        for (const auto &lease : store.read_leases())
          result.push_back(lease.json());
      } else {
        for (const auto &lease : store.upload_leases())
          result.push_back(lease.json());
      }
      print(result);
    } else if (sub == "release") {
      args.count(1);
      if (reads)
        store.release_read_lease(args.values[0]);
      else
        store.release_upload_lease(args.values[0]);
      print(Json{{"released", args.values[0]}});
    } else if (sub == "prune") {
      args.count(0);
      print(Json{{"removed", reads ? store.prune_read_leases() : store.prune_upload_leases()}});
    } else
      throw Error("unknown lease command");
  } else if (command == "serve") {
    net::ServeOptions limits;
    limits.object_compression = !args.flag("--no-compression");
    limits.require_upload_lease = args.flag("--require-lease");
    if (auto seconds = args.option("--lease-seconds"))
      limits.upload_lease_seconds = integer(*seconds);
    if (auto seconds = args.option("--read-lease-seconds"))
      limits.read_lease_seconds = integer(*seconds);
    auto listen = args.option("--listen").value_or("127.0.0.1:7437");
    args.count(0);
    require(token.has_value(), "set SYRNCK_TOKEN");
    net::serve(store, *token, listen, language, limits);
  } else if (command == "agent") {
    if (sub == "sessions") {
      args.count(0);
      auto result = Json::array();
      for (const auto &session : store.transfer_sessions())
        result.push_back(session.json());
      print(result);
      require(std::cout.good(), "output write failed");
      return 0;
    }
    if (sub == "forget") {
      args.count(1);
      store.remove_transfer_session(args.values[0]);
      print(Json{{"discarded", args.values[0]}});
      require(std::cout.good(), "output write failed");
      return 0;
    }
    if (sub == "prune") {
      auto remove = args.flag("--delete");
      args.count(0);
      print(store.prune_transfer_sessions(remove, {control->token(), observe}));
      require(std::cout.good(), "output write failed");
      return 0;
    }
    auto limits = transfer_options(args);
    args.count(sub == "resume" ? 2 : 3);
    require(token.has_value(), "set SYRNCK_TOKEN");
    net::Remote remote(args.values[0], *token, ca_path);
    if (sub == "resume") {
      print(remote.resume(store, args.values[1], {control->token(), observe}, limits));
    } else if (sub == "push") {
      auto refs = store.refs();
      require(refs.contains(args.values[1]), "unknown local reference");
      print(remote.push(store, refs.at(args.values[1]), args.values[2], {control->token(), observe}, limits));
    } else if (sub == "pull") {
      auto refs = remote.refs({control->token(), observe}, limits);
      require(refs.contains(args.values[1]), "unknown remote reference");
      print(remote.pull(store, refs.at(args.values[1]), args.values[2], {control->token(), observe}, limits));
    } else
      throw Error("unknown agent command");
  } else
    throw Error("unknown command; use --help");
  require(std::cout.good(), "output write failed");
  return 0;
}
int entry(Args args) {
  auto language = Language::english;
  std::unique_ptr<cli::OperationControl> control;
  try {
    auto code = args.option("--lang");
    if (!code)
      code = environment("SYRNCK_LANG");
    if (code)
      language = parse_language(*code);
    return run(std::move(args), language, control);
  } catch (const net::PublicationUncertain &error) {
    std::cerr << localize("Error: ", language) << localize(error.what(), language) << '\n';
    return 3;
  } catch (const Cancelled &) {
    std::cerr << localize("Error: ", language) << localize("operation cancelled", language) << '\n';
    return 130;
  } catch (const std::exception &e) {
    auto interrupted = control && control->interrupted();
    std::cerr << localize("Error: ", language)
              << localize(interrupted ? "operation cancelled" : e.what(), language) << '\n';
    return interrupted ? 130 : 1;
  }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  Args args;
  for (int i = 1; i < argc; ++i)
    args.values.push_back(syrnck::utf8(argv[i]));
  return entry(std::move(args));
}
#else
int main(int argc, char **argv) {
  Args args;
  for (int i = 1; i < argc; ++i)
    args.values.emplace_back(argv[i]);
  return entry(std::move(args));
}
#endif
