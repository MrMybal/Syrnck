#include "internal.hpp"
#include "cli_control.hpp"
#include "cli_text_diff.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <syrnck/git.hpp>
#include <syrnck/git_lfs.hpp>
#include <syrnck/git_net.hpp>
#include <syrnck/net.hpp>
#include <syrnck/native_net.hpp>
#include <syrnck/i18n.hpp>
#include <syrnck/asset_locks.hpp>
#include <syrnck/backup.hpp>
#include <syrnck/retention.hpp>
#include <syrnck/bisect.hpp>
#include <syrnck/history.hpp>
#include <syrnck/workspace.hpp>
#include <syrnck/stash.hpp>
#include <syrnck/checkout_retention.hpp>
#include <syrnck/working_index.hpp>
#ifdef _WIN32
#include <Windows.h>
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
  std::vector<std::string> paths() {
    const auto separator = std::find(values.begin(), values.end(), "--");
    for (auto it = values.begin(); it != separator; ++it)
      require(!it->starts_with('-'), "unexpected or missing arguments; use --help");
    count(values.size() - (separator != values.end()));
    std::vector<std::string> result;
    for (const auto &value : values) {
      auto path = path_from_utf8(value).lexically_normal();
      if (!path.empty() && path.filename().empty())
        path = path.parent_path();
      result.push_back(utf8(path));
    }
    return result;
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

#ifdef _WIN32
std::string utf8_cli_text(std::wstring_view wide) {
  require(wide.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()),
          "cannot decode command line text");
  if (wide.empty())
    return {};
  const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  require(size > 0, "cannot decode command line text");
  std::string result(static_cast<std::size_t>(size), '\0');
  require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
                              result.data(), size, nullptr, nullptr) == size,
          "cannot decode command line text");
  return result;
}
#endif
std::string rebase_input_line(std::size_t &total, std::stop_token stop) {
  std::string line;
#ifdef _WIN32
  const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdin)));
  DWORD mode = 0;
  if (GetConsoleMode(handle, &mode)) {
    // Read UTF-16 console input directly without changing the user's code page.
    // Redirected input follows the portable UTF-8 byte path below.
    std::wstring wide;
    for (;;) {
      throw_if_cancelled(stop);
      wchar_t character = 0;
      DWORD count = 0;
      const auto success = ReadConsoleW(handle, &character, 1, &count, nullptr);
      throw_if_cancelled(stop);
      require(success && count == 1, "native rebase interactive input ended before confirmation");
      require(wide.size() < 1024 * 1024 + 80, "native rebase interactive input exceeds size limit");
      if (character == L'\n')
        break;
      wide += character;
    }
    line = utf8_cli_text(wide);
    require(line.size() < 1024 * 1024 + 80 && line.size() + 1 <= max_object_size - total,
            "native rebase interactive input exceeds size limit");
    total += line.size() + 1;
    return line;
  }
#endif
  char c;
  while (std::cin.get(c)) {
    throw_if_cancelled(stop);
    require(++total <= max_object_size && line.size() < 1024 * 1024 + 80,
            "native rebase interactive input exceeds size limit");
    if (c == '\n')
      return line;
    line += c;
  }
  throw Error("native rebase interactive input ended before confirmation");
}
NativeRebaseTodo interactive_rebase_todo(const Store &store, NativeRebaseTodo initial, Language language,
                                         std::stop_token stop) {
  std::cerr << localize("Rebase sources (oldest first):\n", language);
  for (const auto &step : initial.steps) {
    const auto metadata = store.metadata(Kind::revision, step.source);
    auto message = metadata["message"].get<std::string>();
    auto end = std::min<std::size_t>(80, message.size());
    if (end < message.size())
      while (end && (static_cast<unsigned char>(message[end]) & 0xc0) == 0x80)
        --end;
    std::cerr << step.source << ' ' << Json(message.substr(0, end)).dump() << '\n';
  }
  std::cerr << localize("Enter action REVISION per line; reword adds a message. Actions: pick, drop, reword, "
                        "squash, fixup.\nEnd with '.'; '.' alone keeps the original sequence.\n",
                        language)
            << std::flush;
  require(std::cerr.good(), "output write failed");
  auto result = initial;
  result.steps.clear();
  std::size_t total = 0;
  for (;;) {
    throw_if_cancelled(stop);
    auto line = rebase_input_line(total, stop);
    if (line.ends_with('\r'))
      line.pop_back();
    if (line == ".") {
      if (result.steps.empty())
        return initial;
      result.validate();
      return result;
    }
    if (line.empty())
      continue;
    require(result.steps.size() < 100000, "native rebase todo step limit exceeded");
    const auto space = line.find(' ');
    require(space != line.npos && line.size() >= space + 65, "invalid native rebase interactive line");
    NativeRebaseTodoStep step{line.substr(space + 1, 64), line.substr(0, space), {}};
    if (step.action == "reword") {
      require(line.size() >= space + 66 && line[space + 65] == ' ', "invalid native rebase interactive line");
      step.message = line.substr(space + 66);
    } else
      require(line.size() == space + 65, "invalid native rebase interactive line");
    result.steps.push_back(std::move(step));
  }
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
TextDiffOptions text_diff_options(Args &args) {
  TextDiffOptions limits;
  if (const auto context = args.option("--context")) {
    const auto value = integer(*context);
    require(value <= 100, "text diff context must be in 0..100");
    limits.context = static_cast<std::size_t>(value);
  }
  auto budget = [&](std::string_view name, std::size_t &field) {
    if (const auto value = args.option(name)) {
      const auto count = integer(*value);
      require(count <= std::numeric_limits<std::size_t>::max(),
              "text diff budget exceeds platform size limit");
      field = static_cast<std::size_t>(count);
    }
  };
  budget("--max-file-bytes", limits.max_file_bytes);
  budget("--max-lines", limits.max_lines);
  budget("--max-work", limits.max_work);
  budget("--max-output-bytes", limits.max_output_bytes);
  limits.validate();
  return limits;
}
net::TransferOptions transfer_options(Args &args) {
  net::TransferOptions limits;
  if (const auto path = args.option("--lock-claims")) {
    const auto value = parse_json(read_bytes(path_from_utf8(*path), 1024 * 1024));
    require(value.is_array() && value.size() <= 1024, "invalid server asset lock claims");
    for (const auto &claim : value) {
      require(claim.is_object() && claim.size() == 2 && claim.contains("path") && claim["path"].is_string() &&
                  claim.contains("fence") && claim["fence"].is_number_unsigned(),
              "invalid server asset lock claims");
      limits.asset_locks.push_back({claim["path"].get<std::string>(), claim["fence"].get<std::uint64_t>()});
    }
  }
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
  get MANIFESTE NOUVEAU_FICHIER [--offset N --length N]   Reconstruire un fichier ou une plage
  compare SOURCE CIBLE [--summary]   Plan de réutilisation de blocs entre manifestes
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
  native commit [-m MESSAGE] [--branch BRANCHE] [--directory .]
                [--workers N] [--buffer-mib N]
  native commit --staged -m MESSAGE [--branch BRANCHE]
  native commit --amend [--staged | --message-only] -m MESSAGE
                       [--branch BRANCHE] [--expect ID] [--directory .]
    Archive l'ancien commit ; conserve ses parents. --expect exige l'ID exact.
  native add CHEMIN... [--branch BRANCHE] [--directory .]
  native add-patch-plan CHEMIN --patch FICHIER [--branch NOM]
  native add-patch CHEMIN --patch FICHIER --expect-plan ID [--branch NOM]
  native add-lines-plan CHEMIN [--branch NOM] [--directory .]
  native add-lines CHEMIN --expect-plan ID --lines 1,3 [--branch NOM]
  native reset-lines-plan CHEMIN [--branch NOM]
  native reset-lines CHEMIN --expect-plan ID --lines 1,3 [--branch NOM]
  native reset-hunks-plan CHEMIN [--branch NOM]
  native reset-hunks CHEMIN --expect-plan ID --hunks 1,3 [--branch NOM]
  native add-hunks-plan CHEMIN [--branch NOM] [--directory .]
  native add-hunks CHEMIN --expect-plan ID --hunks 1,3 [--branch NOM]
  native staged [--branch BRANCHE]   Inspecter les changements préparés
  native rebase-index-plan [--branch BRANCHE] [--verify] [--text-merge]
  native rebase-index [--branch BRANCHE] [--expect ID] [--expect-index ID] [--text-merge]
    --resolutions FICHIER_JSON : choix liés à la tête, à l'index et à sa base.
  native reset [CHEMIN...] [--branch BRANCHE]
  native reset-plan RÉVISION --soft|--mixed|--hard [--branch BRANCHE] [--verify]
  native reset RÉVISION --soft|--mixed|--hard [--branch BRANCHE] [--expect ID]
    Déplace la branche sans écrire les fichiers ; archive l'historique et l'index.
  native squash-plan NOMBRE [--branch BRANCHE] [--verify]
  native squash NOMBRE -m MESSAGE [--branch BRANCHE] [--expect ID]
    Regroupe les derniers commits linéaires ; conserve le snapshot et l'ancien historique.
  native archives [--kind reset|reset-index|rebase-index|stage-index|squash|amend|checkout|detached]
                  [--after RÉFÉRENCE] [--limit 100] [--verify]
  native archive-show RÉFÉRENCE [--verify]
  native status [--branch BRANCHE | --revision RÉVISION] [--directory .] [--indexed]
  native status --combined [--branch BRANCHE] [--directory .] [--indexed]
  native index-clear
  native checkout-plan RÉVISION [--branch BRANCHE] [--directory .] [--verify]
  native checkout RÉVISION [--branch BRANCHE] [--directory .]
  native checkout-receipts [--after ID] [--limit 100] [--verify]
  native checkout-status [--id TRANSACTION]
  native checkout-recover [--abort]
  native head                       Afficher la branche courante
  native switch-plan BRANCHE [--directory .] [--verify]
  native switch BRANCHE [--directory .]
  native diff SOURCE CIBLE          Comparer deux révisions natives
  native merge-text BASE COURANT ENTRANT CHEMIN [--text]
                                   Fusion textuelle vérifiée en lecture seule
  native diff-text SOURCE CIBLE CHEMIN [--context 3] [--unified]
  native diff-text --staged CHEMIN [--branch NOM] [--context 3] [--unified]
  native diff-text --working CHEMIN [--branch NOM | --revision SOURCE]
                   [--directory .] [--context 3] [--unified]
    Limites du diff : --max-file-bytes N --max-lines N --max-work N --max-output-bytes N
  native branch NOM RÉVISION
  native branches [--after NOM] [--limit 100] [--verify]
  native branch-delete-plan NOM [--archive-index] [--verify]
  native branch-delete NOM --expect ID [--archive-index] [--expect-index ID]
  native branch-archives [--after ID] [--limit 100] [--verify]
  native branch-archive ID [--verify]
  native branch-restore-plan ID [--as NOM] [--verify]
  native branch-restore ID [--as NOM]
  native tag NOM [RÉVISION] [-m MESSAGE]   Nommer une version (HEAD par défaut)
  native tags [--after NOM] [--limit 100] | tag-show NOM [--verify]
  native tag-delete NOM --expect RÉVISION
  native tag-archive ARCHIVE [--verify] | tag-restore ARCHIVE
  native log [--branch BRANCHE] [--limit 50]
  native ancestry SOURCE CIBLE [--max-revisions 100000] [--max-links 1000000]
    Relations d'ascendance et tous les meilleurs ancêtres communs.
  native merge-plan RÉVISION [--base RÉVISION] [--allow-unrelated-histories] [--text-merge]
                    [--branch BRANCHE] [--directory .] [--verify]
                    [--no-ff | --ff-only] [--max-revisions 100000] [--max-links 1000000]
  native merge RÉVISION [-m MESSAGE] [--expect ID] [mêmes options sauf --verify]
  native rebase-plan BASE --onto RÉVISION [--branch BRANCHE] [--directory .] [--text-merge]
                     [--max-commits 1000] [--verify]
  native rebase BASE --onto RÉVISION [mêmes options sauf --verify] [--expect ID]
  native rebase-todo BASE --onto RÉVISION    Exporter la séquence modifiable
    --todo FICHIER_JSON ou --interactive : ordre, pick/drop/reword/squash/fixup.
    --resolutions FICHIER_JSON : choix par étape liés à la tête et aux deux bases.
    Merge/rejeu : --resolutions FICHIER_JSON applique les choix liés au plan.
  native cherry-pick-plan RÉVISION [--mainline N] [--verify] [--text-merge]
  native revert-plan RÉVISION [--mainline N] [--verify] [--text-merge]
    [--branch BRANCHE] [--directory .] ; calcul et contrôle sans écriture.
  native cherry-pick RÉVISION -m MESSAGE [--mainline N] [--expect ID] [--text-merge]
  native revert RÉVISION -m MESSAGE [--mainline N] [--expect ID] [--text-merge]
    [--branch BRANCHE] [--directory .] ; nouveau commit et checkout récupérable.
  native export RÉVISION NOUVEAU_DOSSIER
  native detach-history -m MESSAGE [--branch BRANCHE]
  git install [DOSSIER] [--encrypted]
  git catalog COMMIT [--directory CHEMIN]   Inspecter les pointeurs du commit
  git retention-scan [--directory CHEMIN]   Inventorier les pointeurs de toutes les racines Git
  git retention [--apply] [--directory CHEMIN]   Auditer ou ajouter les protections vérifiées
  git prepare-assets COMMIT [--directory CHEMIN]   Vérifier et conserver ses assets
  git push-assets URL COMMIT | git pull-assets URL COMMIT [--requests N]
  git configure-remote NOM URL | git install-hook [--directory CHEMIN]
  git configure-checkout local|download|skip [--remote NOM] [--directory CHEMIN]
  git clean [--workers N] [--buffer-mib N] | git smudge
  git filter-process [--checkout-policy]   Protocole Git v2, plusieurs fichiers par processus
  git lfs-preview COMMIT [--lfs-objects CHEMIN]   Auditer les objets LFS locaux
  git lfs-history-preview COMMIT [--max-commits 10000] [--strip-signatures]
  git migrate-lfs-history COMMIT --branch NOUVELLE [--strip-signatures]
  git migrate-lfs COMMIT --branch NOUVELLE [--lfs-objects CHEMIN]
  serve [--listen 127.0.0.1:7437]      HTTP local ; publier derrière un proxy HTTPS
        [--lease-seconds 3600] [--read-lease-seconds 3600] [--require-lease]
  leases list | release ID | prune [--read]   Gérer les baux du store serveur
  token-hash                         Empreinte du jeton SYRNCK_TOKEN (sans store)
  backup create NOUVEAU_STORE | verify | restore NOUVEAU_STORE
  backup repair STORE_SAUVEGARDE [--apply]   Prévisualiser/réparer les objets absents
  backup repair-copy-plan STORE_SAUVEGARDE  Vérifier les objets récupérables
  backup repair-copy STORE_SAUVEGARDE NOUVEAU_STORE  Reconstruire une copie vérifiée
  backup checkout-retire-plan ID... --from SAUVEGARDE [--include-source-archives]
  backup checkout-retire ID... --from SAUVEGARDE --expect-plan PLAN [--include-source-archives]
  backup retire-plan ARCHIVE... --from SAUVEGARDE
  backup retire ARCHIVE... --from SAUVEGARDE --expect-plan ID
  locks identity --remote URL        Afficher l’identité authentifiée
  locks list [--after CHEMIN] [--limit 100]   Lister les verrous d’assets
  locks acquire-plan|acquire --owner ID [--seconds 3600] CHEMIN...
  locks renew --owner ID --fence N [--seconds 3600] CHEMIN...
  locks release --owner ID --fence N CHEMIN... | locks prune [--delete]
    --remote URL utilise l’identité serveur ; omettre --owner.
  serve --access FICHIER             Identités, droits et règles de branches
  native push URL [--branch NOM] [--remote-branch NOM] [--lock-claims FICHIER]
  native fetch URL [--remote-branch NOM] [--remote-name origin]
  native stash push [-m MESSAGE] [--include-untracked]
  native stash list [--before GÉNÉRATION] [--limit N]
  native stash show|archive ID [--verify]
  native stash apply-plan ID [--index] [--text-merge] [--verify]
  native stash apply|pop ID --expect-plan PLAN [--index]
  native stash drop ID --expect-record OBJET | restore ID
  native bisect-start BON MAUVAIS | bisect-status
  native bisect-good|bisect-bad|bisect-skip RÉVISION --expect-state ID
  native bisect-reset --expect-state ID   Archiver la recherche sans toucher aux fichiers
  native diff SOURCE CIBLE --renames     Détecter les renommages de contenu exact
  native log-file RÉVISION CHEMIN [--limit 100] [--max-revisions 1000] [--no-follow]
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
Branche native : --branch remplace la branche courante (main sans HEAD explicite).
Révisions natives : ID, HEAD, branche/tag non ambigu, heads/NOM ou tags/NOM.
Progression : --progress ou --progress-json sur stderr pour put, get, compare, pack, gc,
              fsck, native commit/add/add-patch-plan/add-patch/add-hunks-plan/add-hunks/reset-hunks-plan/reset-hunks/add-lines-plan/add-lines/reset-lines-plan/reset-lines/staged/reset/status/checkout-plan/checkout/checkout-recover/switch-plan/switch/diff/diff-text/merge-text/export,
              git clean/smudge/filter-process,
              native tag/tags/tag-show/tag-delete/tag-archive/tag-restore,
              native branch/branches/branch-delete-plan/branch-delete/branch-archives/branch-archive,
              native branch-restore-plan/branch-restore,
              native ancestry/merge-plan/merge/cherry-pick-plan/revert-plan/cherry-pick/revert,
              native reset-plan/squash-plan/squash/archives/archive-show,
              native rebase-plan/rebase/rebase-index-plan/rebase-index,
              git lfs-preview/migrate-lfs/retention-scan/retention et agent push/pull/resume/prune.
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
  get MANIFEST NEW_FILE [--offset N --length N]   Reconstruct a file or byte range
  compare SOURCE TARGET [--summary]  Chunk reuse plan between manifests
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
  native commit [-m MESSAGE] [--branch BRANCH] [--directory .]
                [--workers N] [--buffer-mib N]
  native commit --staged -m MESSAGE [--branch BRANCH]
  native commit --amend [--staged | --message-only] -m MESSAGE
                       [--branch BRANCH] [--expect ID] [--directory .]
    Archives the previous tip and preserves its parents. --expect requires an exact ID.
  native add PATH... [--branch BRANCH] [--directory .]
  native add-patch-plan PATH --patch FILE [--branch NAME]
  native add-patch PATH --patch FILE --expect-plan ID [--branch NAME]
  native add-lines-plan PATH [--branch NAME] [--directory .]
  native add-lines PATH --expect-plan ID --lines 1,3 [--branch NAME]
  native reset-lines-plan PATH [--branch NAME]
  native reset-lines PATH --expect-plan ID --lines 1,3 [--branch NAME]
  native reset-hunks-plan PATH [--branch NAME]
  native reset-hunks PATH --expect-plan ID --hunks 1,3 [--branch NAME]
  native add-hunks-plan PATH [--branch NAME] [--directory .]
  native add-hunks PATH --expect-plan ID --hunks 1,3 [--branch NAME]
  native staged [--branch BRANCH]    Inspect staged changes
  native rebase-index-plan [--branch BRANCH] [--verify] [--text-merge]
  native rebase-index [--branch BRANCH] [--expect ID] [--expect-index ID] [--text-merge]
    --resolutions JSON_FILE: choices bound to the head, index and its baseline.
  native reset [PATH...] [--branch BRANCH]
  native reset-plan REVISION --soft|--mixed|--hard [--branch BRANCH] [--verify]
  native reset REVISION --soft|--mixed|--hard [--branch BRANCH] [--expect ID]
    Moves the branch without writing files; archives previous history and index.
  native squash-plan COUNT [--branch BRANCH] [--verify]
  native squash COUNT -m MESSAGE [--branch BRANCH] [--expect ID]
    Collapses linear tip commits, preserving the snapshot and archived history.
  native archives [--kind reset|reset-index|rebase-index|stage-index|squash|amend|checkout|detached]
                  [--after REFERENCE] [--limit 100] [--verify]
  native archive-show REFERENCE [--verify]
  native status [--branch BRANCH | --revision REVISION] [--directory .] [--indexed]
  native status --combined [--branch BRANCH] [--directory .] [--indexed]
  native index-clear
  native checkout-plan REVISION [--branch BRANCH] [--directory .] [--verify]
  native checkout REVISION [--branch BRANCH] [--directory .]
  native checkout-receipts [--after ID] [--limit 100] [--verify]
  native checkout-status [--id TRANSACTION]
  native checkout-recover [--abort]
  native head                       Show the current branch
  native switch-plan BRANCH [--directory .] [--verify]
  native switch BRANCH [--directory .]
  native diff SOURCE TARGET         Compare native revision snapshots
  native merge-text BASE CURRENT INCOMING PATH [--text]
                                   Preview a verified three-way text merge
  native diff-text SOURCE TARGET PATH [--context 3] [--unified]
  native diff-text --staged PATH [--branch NAME] [--context 3] [--unified]
  native diff-text --working PATH [--branch NAME | --revision SOURCE]
                   [--directory .] [--context 3] [--unified]
    Diff limits: --max-file-bytes N --max-lines N --max-work N --max-output-bytes N
  native branch NAME REVISION
  native branches [--after NAME] [--limit 100] [--verify]
  native branch-delete-plan NAME [--archive-index] [--verify]
  native branch-delete NAME --expect ID [--archive-index] [--expect-index ID]
  native branch-archives [--after ID] [--limit 100] [--verify]
  native branch-archive ID [--verify]
  native branch-restore-plan ID [--as NAME] [--verify]
  native branch-restore ID [--as NAME]
  native tag NAME [REVISION] [-m MESSAGE]   Label a version (defaults to HEAD)
  native tags [--after NAME] [--limit 100] | tag-show NAME [--verify]
  native tag-delete NAME --expect REVISION
  native tag-archive ARCHIVE [--verify] | tag-restore ARCHIVE
  native log [--branch BRANCH] [--limit 50]
  native ancestry SOURCE TARGET [--max-revisions 100000] [--max-links 1000000]
    Ancestry relations and every best common ancestor.
  native merge-plan REVISION [--base REVISION] [--allow-unrelated-histories] [--text-merge]
                    [--branch BRANCH] [--directory .] [--verify]
                    [--no-ff | --ff-only] [--max-revisions 100000] [--max-links 1000000]
  native merge REVISION [-m MESSAGE] [--expect ID] [same options except --verify]
  native rebase-plan BASE --onto REVISION [--branch BRANCH] [--directory .] [--text-merge]
                     [--max-commits 1000] [--verify]
  native rebase BASE --onto REVISION [same options except --verify] [--expect ID]
  native rebase-todo BASE --onto REVISION    Export an editable sequence
    --todo JSON_FILE or --interactive: order, pick/drop/reword/squash/fixup.
    --resolutions JSON_FILE: per-step choices bound to the head and both bases.
    Merge/replay: --resolutions JSON_FILE applies choices bound to the plan.
  native cherry-pick-plan REVISION [--mainline N] [--verify] [--text-merge]
  native revert-plan REVISION [--mainline N] [--verify] [--text-merge]
    [--branch BRANCH] [--directory .]; read-only replay and workspace preflight.
  native cherry-pick REVISION -m MESSAGE [--mainline N] [--expect ID] [--text-merge]
  native revert REVISION -m MESSAGE [--mainline N] [--expect ID] [--text-merge]
    [--branch BRANCH] [--directory .]; new commit and recoverable checkout.
  native export REVISION NEW_DIRECTORY
  native detach-history -m MESSAGE [--branch BRANCH]
  git install [DIRECTORY] [--encrypted]
  git catalog COMMIT [--directory PATH]   Inspect committed asset pointers
  git retention-scan [--directory PATH]   Inventory pointers from all Git roots
  git retention [--apply] [--directory PATH]   Audit or add verified retention pins
  git prepare-assets COMMIT [--directory PATH]   Verify and retain its assets
  git push-assets URL COMMIT | git pull-assets URL COMMIT [--requests N]
  git configure-remote NAME URL | git install-hook [--directory PATH]
  git configure-checkout local|download|skip [--remote NAME] [--directory PATH]
  git clean [--workers N] [--buffer-mib N] | git smudge
  git filter-process [--checkout-policy]   Git protocol v2, multiple files per process
  git lfs-preview COMMIT [--lfs-objects PATH]   Audit local LFS objects
  git lfs-history-preview COMMIT [--max-commits 10000] [--strip-signatures]
  git migrate-lfs-history COMMIT --branch NEW [--strip-signatures]
  git migrate-lfs COMMIT --branch NEW [--lfs-objects PATH]
  serve [--listen 127.0.0.1:7437]      Loopback HTTP; publish behind HTTPS
        [--lease-seconds 3600] [--read-lease-seconds 3600] [--require-lease]
  leases list | release ID | prune [--read]   Manage server store leases
  token-hash                         Hash SYRNCK_TOKEN (no store needed)
  backup create NEW_STORE | verify | restore NEW_STORE
  backup repair BACKUP_STORE [--apply]   Preview/repair missing objects
  backup repair-copy-plan BACKUP_STORE  Inspect recoverable retained objects
  backup repair-copy BACKUP_STORE NEW_STORE  Rebuild a verified Store from both inputs
  backup checkout-retire-plan ID... --from BACKUP [--include-source-archives]
  backup checkout-retire ID... --from BACKUP --expect-plan PLAN [--include-source-archives]
  backup retire-plan ARCHIVE... --from BACKUP
  backup retire ARCHIVE... --from BACKUP --expect-plan ID
  locks identity --remote URL        Show the authenticated identity
  locks list [--after PATH] [--limit 100]   List asset locks
  locks acquire-plan|acquire --owner ID [--seconds 3600] PATH...
  locks renew --owner ID --fence N [--seconds 3600] PATH...
  locks release --owner ID --fence N PATH... | locks prune [--delete]
    --remote URL uses the server identity; omit --owner.
  serve --access FILE                Identities, permissions and branch policies
  native push URL [--branch NAME] [--remote-branch NAME] [--lock-claims FILE]
  native fetch URL [--remote-branch NAME] [--remote-name origin]
  native stash push [-m MESSAGE] [--include-untracked]
  native stash list [--before GENERATION] [--limit N]
  native stash show|archive ID [--verify]
  native stash apply-plan ID [--index] [--text-merge] [--verify]
  native stash apply|pop ID --expect-plan PLAN [--index]
  native stash drop ID --expect-record OBJECT | restore ID
  native bisect-start GOOD BAD | bisect-status
  native bisect-good|bisect-bad|bisect-skip REVISION --expect-state ID
  native bisect-reset --expect-state ID   Archive the search without changing files
  native diff SOURCE TARGET --renames    Detect exact-content renames
  native log-file REVISION PATH [--limit 100] [--max-revisions 1000] [--no-follow]
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
Native branch: --branch overrides the current branch (main without explicit HEAD).
Native revisions: ID, HEAD, unambiguous branch/tag, heads/NAME or tags/NAME.
Progress: --progress or --progress-json on stderr for put, get, compare, pack, gc,
              fsck, native commit/add/add-patch-plan/add-patch/add-hunks-plan/add-hunks/reset-hunks-plan/reset-hunks/add-lines-plan/add-lines/reset-lines-plan/reset-lines/staged/reset/status/checkout-plan/checkout/checkout-recover/switch-plan/switch/diff/diff-text/merge-text/export,
              git clean/smudge/filter-process,
              native tag/tags/tag-show/tag-delete/tag-archive/tag-restore,
              native branch/branches/branch-delete-plan/branch-delete/branch-archives/branch-archive,
              native branch-restore-plan/branch-restore,
              native ancestry/merge-plan/merge/cherry-pick-plan/revert-plan/cherry-pick/revert,
              native reset-plan/squash-plan/squash/archives/archive-show,
              native rebase-plan/rebase/rebase-index-plan/rebase-index,
              git lfs-preview/migrate-lfs/retention-scan/retention and agent push/pull/resume/prune.
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
  if (command == "token-hash") {
    args.count(0);
    require(
        token && token->size() >= 32 && token->size() <= 4096 &&
            std::all_of(token->begin(), token->end(), [](unsigned char c) { return c >= 33 && c <= 126; }),
        "set SYRNCK_TOKEN to at least 32 printable characters");
    std::cout << hash(bytes(*token)) << '\n';
    return 0;
  }
  std::string sub;
  if (command == "git" || command == "native" || command == "agent" || command == "config" ||
      command == "leases" || command == "locks" || command == "backup")
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
  if (command == "init") {
    auto key = read_key();
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
    auto key = read_key();
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
  if (command == "git" &&
      (sub == "catalog" || sub == "lfs-preview" || sub == "lfs-history-preview" || sub == "retention-scan")) {
    auto directory = path_from_utf8(args.option("--directory").value_or("."));
    auto cache = (sub == "lfs-preview" || sub == "lfs-history-preview") ? args.option("--lfs-objects")
                                                                        : std::optional<std::string>{};
    git::LfsHistoryOptions history;
    if (sub == "lfs-history-preview") {
      history.strip_signatures = args.flag("--strip-signatures");
      if (auto value = args.option("--max-commits")) {
        const auto limit = integer(*value);
        require(limit >= 1 && limit <= 100000, "Git LFS history commit limit must be in 1..100000");
        history.max_commits = static_cast<std::size_t>(limit);
      }
    }
    auto text = args.flag("--progress"), json = args.flag("--progress-json");
    require(!(text && json), "choose one progress output mode");
    args.count(sub == "retention-scan" ? 0 : 1);
    control = std::make_unique<cli::OperationControl>(language, json   ? cli::ProgressMode::json
                                                                : text ? cli::ProgressMode::text
                                                                       : cli::ProgressMode::none);
    OperationOptions operation{control->token(), [&](const ProgressEvent &event) { control->report(event); }};
    if (sub == "retention-scan") {
      print(git::retention_inventory(directory, operation).json());
      return 0;
    }
    if (sub == "lfs-preview" || sub == "lfs-history-preview") {
      auto objects =
          git::lfs_objects(directory, cache ? std::optional{path_from_utf8(*cache)} : std::nullopt);
      auto result =
          sub == "lfs-preview"
              ? git::preview_lfs(git::lfs_catalog(directory, args.values[0], operation), objects, operation)
              : git::preview_lfs_history(directory, args.values[0], objects, history, operation);
      auto ready = result["ready"].get<bool>();
      print(result);
      return ready ? 0 : 1;
    }
    print(git::catalog(directory, args.values[0], operation).json());
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
      (sub == "prepare-assets" || sub == "push-assets" || sub == "pull-assets" || sub == "pre-push" ||
       sub == "migrate-lfs" || sub == "migrate-lfs-history" || sub == "retention") &&
      !root_option)
    root = git::common_store(git_directory);
  Store store(root, read_key(), false, store_options);
  auto text_progress = args.flag("--progress"), json_progress = args.flag("--progress-json");
  require(!(text_progress && json_progress), "choose one progress output mode");
  bool cancellable =
      command == "put" || command == "get" || command == "compare" || command == "fsck" ||
      command == "pack" || command == "gc" || command == "locks" || command == "backup" ||
      (command == "native" &&
       (sub == "commit" || sub == "export" || sub == "status" || sub == "diff" || sub == "diff-text" ||
        sub == "merge-text" || sub == "add" || sub == "add-patch-plan" || sub == "add-patch" ||
        sub == "add-hunks-plan" || sub == "add-hunks" || sub == "reset-hunks-plan" || sub == "reset-hunks" ||
        sub == "add-lines-plan" || sub == "add-lines" || sub == "reset-lines-plan" || sub == "reset-lines" ||
        sub == "staged" || sub == "reset" || sub == "reset-plan" || sub == "checkout-plan" ||
        sub == "checkout" || sub == "checkout-recover" || sub == "switch-plan" || sub == "switch" ||
        sub == "tag" || sub == "tags" || sub == "tag-show" || sub == "tag-delete" || sub == "tag-archive" ||
        sub == "tag-restore" || sub == "ancestry" || sub == "cherry-pick-plan" || sub == "revert-plan" ||
        sub == "cherry-pick" || sub == "revert" || sub == "merge-plan" || sub == "merge" ||
        sub == "squash-plan" || sub == "squash" || sub == "archives" || sub == "archive-show" ||
        sub == "rebase-plan" || sub == "rebase-todo" || sub == "rebase" || sub == "rebase-index-plan" ||
        sub == "rebase-index" || sub == "branch" || sub == "branches" || sub == "branch-delete-plan" ||
        sub == "branch-delete" || sub == "branch-archives" || sub == "branch-archive" ||
        sub == "branch-restore-plan" || sub == "branch-restore" || sub == "push" || sub == "fetch" ||
        sub == "checkout-receipts" || sub == "index-clear" || sub == "stash" || sub == "log-file" || sub.starts_with("bisect-"))) ||
      (command == "agent" && (sub == "push" || sub == "pull" || sub == "resume" || sub == "prune")) ||
      (command == "git" && (sub == "clean" || sub == "smudge" || sub == "prepare-assets" ||
                            sub == "push-assets" || sub == "pull-assets" || sub == "pre-push" ||
                            sub == "migrate-lfs" || sub == "migrate-lfs-history" || sub == "retention"));
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
    auto offset = args.option("--offset"), length = args.option("--length");
    require(offset.has_value() == length.has_value(), "--offset and --length must be supplied together");
    args.count(2);
    OperationOptions operation{control->token(), [&](const ProgressEvent &event) {
                                 auto contextual = event;
                                 contextual.path = args.values[1];
                                 control->report(contextual);
                               }};
    auto destination = path_from_utf8(args.values[1]);
    if (offset)
      store.restore_file_range(args.values[0], destination, integer(*offset), integer(*length), operation);
    else
      store.restore_file(args.values[0], destination, operation);
  } else if (command == "compare") {
    auto summary = args.flag("--summary");
    args.count(2);
    auto result = store.compare_files(args.values[0], args.values[1], {control->token(), observe});
    if (summary) {
      result.erase("blocks");
      result.erase("needed_chunks");
    }
    print(result);
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
    print(encode_refs(store.refs()));
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
    auto selected_branch = [&] {
      const auto branch = args.option("--branch");
      return branch ? *branch : store.native_head()["branch"].get<std::string>();
    };
    auto workspace = [&] {
      const auto directory = path_from_utf8(args.option("--directory").value_or("."));
      const auto head = store.native_head();
      require(head["directory"].is_null() ||
                  utf8(fs::canonical(directory)) == head["directory"].get<std::string>(),
              "native HEAD belongs to another workspace");
      return directory;
    };
    auto resolve = [&](std::string_view selector) {
      return store.resolve_native_revision(selector, {control ? control->token() : std::stop_token{}});
    };
    auto resolutions_input = [&]() -> std::optional<NativeResolutionSet> {
      if (const auto path = args.option("--resolutions")) {
        throw_if_cancelled(control->token());
        const auto data = read_bytes(path_from_utf8(*path), max_object_size);
        return NativeResolutionSet::decode(data);
      }
      return {};
    };
    if (sub == "checkout-receipts") {
      const auto after = args.option("--after");
      const auto limit = integer(args.option("--limit").value_or("100"));
      require(limit >= 1 && limit <= 1000, "native checkout receipt page limit must be in 1..1000");
      const bool verify = args.flag("--verify");
      args.count(0);
      print(native_checkout_receipts(store, after, static_cast<std::size_t>(limit), verify,
                                     {control->token(), observe}));
    } else if (sub == "stash") {
      NativeStash saved(store);
      const OperationOptions operation{control->token(), observe};
      const auto action = args.pop();
      if (action == "push") {
        const auto directory = workspace();
        const auto branch = selected_branch();
        const auto message = args.option("--message", "-m").value_or("Work in progress");
        const bool untracked = args.flag("--include-untracked");
        args.count(0);
        print(saved.save(directory, branch, message, {untracked}, operation));
      } else if (action == "list") {
        const auto before = args.option("--before");
        const auto limit = integer(args.option("--limit").value_or("100"));
        require(limit >= 1 && limit <= 1000, "native stash page limit must be in 1..1000");
        args.count(0);
        print(saved.list(before ? std::optional{integer(*before)} : std::optional<std::uint64_t>{},
                         static_cast<std::size_t>(limit), operation));
      } else if (action == "show" || action == "archive") {
        const bool verify = args.flag("--verify");
        args.count(1);
        print(action == "show" ? saved.show(args.values[0], verify, operation)
                               : saved.archived(args.values[0], verify, operation));
      } else if (action == "drop") {
        const auto expected = args.option("--expect-record");
        require(expected.has_value(), "native stash drop requires --expect-record");
        args.count(1);
        print(saved.drop(args.values[0], *expected, operation));
      } else if (action == "restore") {
        args.count(1);
        print(saved.restore(args.values[0], operation));
      } else {
        require(action == "apply-plan" || action == "apply" || action == "pop",
                "unknown native stash command");
        const bool planning = action == "apply-plan";
        const auto directory = workspace();
        const auto branch = selected_branch();
        const bool verify = planning && args.flag("--verify");
        NativeStashApplyOptions settings;
        settings.restore_index = args.flag("--index");
        settings.resolutions = resolutions_input();
        if (const auto input = args.option("--index-resolutions"))
          settings.index_resolutions =
              NativeResolutionSet::decode(read_bytes(path_from_utf8(*input), max_object_size));
        if (args.flag("--text-merge"))
          settings.text_merge = text_diff_options(args);
        const auto expected = planning ? std::optional<std::string>{} : args.option("--expect-plan");
        require(planning || expected.has_value(), "native stash apply requires --expect-plan");
        args.count(1);
        if (planning) {
          const auto plan = saved.plan_apply(directory, branch, args.values[0], settings, verify, operation);
          print(plan);
          return plan["ready"].get<bool>() ? 0 : 1;
        }
        print(
            saved.apply(directory, branch, args.values[0], *expected, action == "pop", settings, operation));
      }
    } else if (sub.starts_with("bisect-")) {
      NativeBisect search(store);
      const OperationOptions operation{control->token(), observe};
      if (sub == "bisect-start") {
        args.count(2);
        print(search.start(resolve(args.values[0]), resolve(args.values[1]), operation));
      } else if (sub == "bisect-status") {
        args.count(0);
        print(search.status(operation));
      } else {
        const auto expected = args.option("--expect-state");
        require(expected.has_value(), "native bisect requires --expect-state");
        if (sub == "bisect-reset") {
          args.count(0);
          print(search.reset(*expected, operation));
        } else {
          require(sub == "bisect-good" || sub == "bisect-bad" || sub == "bisect-skip",
                  "unknown native bisect command");
          args.count(1);
          print(search.mark(sub.substr(7), resolve(args.values[0]), *expected, operation));
        }
      }
    } else if (sub == "push" || sub == "fetch") {
      const auto branch = selected_branch();
      const auto remote_branch = args.option("--remote-branch").value_or(branch);
      const auto remote_name = args.option("--remote-name").value_or("origin");
      const auto limits = transfer_options(args);
      args.count(1);
      require(token.has_value(), "set SYRNCK_TOKEN");
      net::Remote remote(args.values[0], *token, ca_path);
      print(sub == "push" ? net::push_native_branch(store, remote, branch, remote_branch,
                                                    {control->token(), observe}, limits)
                          : net::fetch_native_branch(store, remote, remote_branch, remote_name,
                                                     {control->token(), observe}, limits));
    } else if (sub == "commit") {
      const auto staged = args.flag("--staged");
      const auto amend = args.flag("--amend");
      const auto message_only = args.flag("--message-only");
      const auto expected = args.option("--expect");
      require(amend || (!message_only && !expected), "--message-only and --expect require --amend");
      require(!staged || !message_only, "choose either --staged or --message-only");
      auto branch = selected_branch();
      auto message = args.option("--message", "-m");
      require(message.has_value(), "--message is required");
      const auto tip = amend ? (expected ? *expected : resolve("heads/" + branch)) : std::string{};
      if (message_only) {
        args.count(0);
        std::cout << store.amend_message(branch, tip, *message, {control->token(), observe}) << '\n';
      } else if (staged) {
        args.count(0);
        std::cout << (amend ? store.amend_staged(branch, tip, *message, {control->token(), observe})
                            : store.commit_staged(branch, *message, {control->token(), observe}))
                  << '\n';
      } else {
        auto options = ingestion_options(args, store);
        options.stop = control->token();
        options.progress = observe;
        auto directory = workspace();
        args.count(0);
        std::cout << (amend ? store.amend_directory(directory, branch, tip, *message, options)
                            : store.commit_directory(directory, branch, *message, options))
                  << '\n';
      }
    } else if (sub == "add") {
      auto options = ingestion_options(args, store);
      options.stop = control->token();
      options.progress = observe;
      const auto directory = workspace();
      const auto branch = selected_branch();
      std::cout << store.stage_directory(directory, branch, args.paths(), options) << '\n';
    } else if (sub == "add-patch-plan" || sub == "add-patch") {
      const bool planning = sub == "add-patch-plan";
      const auto limits = text_diff_options(args);
      const auto branch = selected_branch();
      const auto input = args.option("--patch");
      require(input.has_value(), "native patch command requires --patch FILE");
      const auto expected = planning ? std::optional<std::string>{} : args.option("--expect-plan");
      if (!planning)
        require(expected.has_value(), "native add-patch requires --expect-plan");
      args.count(1);
      const auto patch = read_bytes(path_from_utf8(*input), limits.max_output_bytes);
      if (planning) {
        const auto plan =
            store.plan_stage_patch(branch, args.values[0], patch, limits, {control->token(), observe});
        print(plan);
        return plan["ready"].get<bool>() ? 0 : 1;
      }
      print(store.stage_patch(branch, args.values[0], patch, *expected, limits, {control->token(), observe}));
    } else if (sub == "add-hunks-plan" || sub == "add-hunks" || sub == "reset-hunks-plan" ||
               sub == "reset-hunks" || sub == "add-lines-plan" || sub == "add-lines" ||
               sub == "reset-lines-plan" || sub == "reset-lines") {
      const bool by_line = sub.find("-lines") != std::string::npos;
      const bool resetting = sub.starts_with("reset-");
      const bool planning = sub.ends_with("-plan");
      const auto limits = text_diff_options(args);
      const auto directory = resetting ? fs::path{} : workspace();
      const auto branch = selected_branch();
      if (planning) {
        args.count(1);
        Json report;
        if (by_line)
          report = resetting
                       ? store.plan_reset_lines(branch, args.values[0], limits, {control->token(), observe})
                       : store.plan_stage_lines(directory, branch, args.values[0], limits,
                                                {control->token(), observe});
        else
          report = resetting
                       ? store.plan_reset_hunks(branch, args.values[0], limits, {control->token(), observe})
                       : store.plan_stage_hunks(directory, branch, args.values[0], limits,
                                                {control->token(), observe});
        print(report);
        return report["ready"].get<bool>() ? 0 : 1;
      }
      const auto expected = args.option("--expect-plan"),
                 selection = args.option(by_line ? "--lines" : "--hunks");
      if (by_line && resetting)
        require(expected && selection, "native reset-lines requires --expect-plan and --lines");
      else if (by_line)
        require(expected && selection, "native add-lines requires --expect-plan and --lines");
      else if (resetting)
        require(expected && selection, "native reset-hunks requires --expect-plan and --hunks");
      else
        require(expected && selection, "native add-hunks requires --expect-plan and --hunks");
      args.count(1);
      std::vector<std::size_t> numbers;
      for (std::size_t pos = 0;;) {
        const auto comma = selection->find(',', pos);
        const auto value = integer(selection->substr(pos, comma == std::string::npos ? comma : comma - pos));
        require(value >= 1 && value <= 100000, "native text selection number is out of range");
        require(numbers.size() < 100000, "native text selection exceeds 100000 numbers");
        numbers.push_back(static_cast<std::size_t>(value));
        if (comma == std::string::npos)
          break;
        pos = comma + 1;
      }
      if (by_line)
        print(resetting ? store.reset_lines(branch, args.values[0], *expected, numbers, limits,
                                            {control->token(), observe})
                        : store.stage_lines(directory, branch, args.values[0], *expected, numbers, limits,
                                            {control->token(), observe}));
      else
        print(resetting ? store.reset_hunks(branch, args.values[0], *expected, numbers, limits,
                                            {control->token(), observe})
                        : store.stage_hunks(directory, branch, args.values[0], *expected, numbers, limits,
                                            {control->token(), observe}));
    } else if (sub == "staged") {
      const auto branch = selected_branch();
      args.count(0);
      print(store.staged_status(branch, {control->token(), observe}));
    } else if (sub == "rebase-index-plan" || sub == "rebase-index") {
      const bool planning = sub == "rebase-index-plan";
      const auto branch = selected_branch();
      const bool verify = planning && args.flag("--verify");
      const auto expected = planning ? std::optional<std::string>{} : args.option("--expect");
      const auto expected_index = planning ? std::optional<std::string>{} : args.option("--expect-index");
      NativeStageRebaseOptions settings;
      settings.resolutions = resolutions_input();
      if (args.flag("--text-merge"))
        settings.text_merge = text_diff_options(args);
      args.count(0);
      if (planning) {
        const auto plan =
            store.plan_rebase_stage_with_options(branch, settings, verify, {control->token(), observe});
        print(plan);
        return plan["ready"].get<bool>() ? 0 : 1;
      }
      const auto all = store.refs();
      const auto index = all.find("staging/" + branch);
      require(index != all.end() && index->second.kind == Kind::revision, "no native staging snapshot");
      const auto head = expected ? *expected : resolve("heads/" + branch);
      const auto snapshot = expected_index ? *expected_index : index->second.id;
      print(store.rebase_stage_with_options(branch, head, snapshot, settings, {control->token(), observe}));
    } else if (sub == "reset" || sub == "reset-plan") {
      const auto branch = selected_branch();
      const bool soft = args.flag("--soft"), mixed = args.flag("--mixed"), planning = sub == "reset-plan";
      const bool hard = args.flag("--hard");
      require(!(soft && mixed), "choose either --soft or --mixed");
      require(!hard || (!soft && !mixed), "choose only one native reset mode");
      if (hard) {
        const auto directory = workspace();
        const bool verify = planning && args.flag("--verify");
        const auto expected = planning ? std::optional<std::string>{} : args.option("--expect");
        const auto expected_plan = planning ? std::optional<std::string>{} : args.option("--expect-plan");
        args.count(1);
        const auto target = resolve(args.values[0]);
        if (planning) {
          const auto plan =
              plan_native_hard_reset(store, directory, branch, target, verify, {control->token(), observe});
          print(plan);
          return plan["ready"].get<bool>() ? 0 : 1;
        }
        const auto tip = expected ? *expected : resolve("heads/" + branch);
        const auto plan_id = expected_plan ? *expected_plan
                                           : plan_native_hard_reset(store, directory, branch, target, false,
                                                                    {control->token(), observe})["plan"]
                                                 .get<std::string>();
        print(native_hard_reset(store, directory, branch, tip, target, plan_id, {control->token(), observe}));
      } else if (soft || mixed || planning) {
        require(soft || mixed, "native reset-plan requires --soft or --mixed");
        const bool verify = planning && args.flag("--verify");
        const auto expected = planning ? std::optional<std::string>{} : args.option("--expect");
        args.count(1);
        const auto target = resolve(args.values[0]);
        const auto mode = soft ? NativeResetMode::soft : NativeResetMode::mixed;
        if (planning) {
          const auto plan =
              store.plan_reset_branch(branch, target, mode, verify, {control->token(), observe});
          print(plan);
          return plan["ready"].get<bool>() ? 0 : 1;
        }
        const auto tip = expected ? *expected : resolve("heads/" + branch);
        print(store.reset_branch(branch, tip, target, mode, {control->token(), observe}));
      } else {
        const auto snapshot = store.reset_stage(branch, args.paths(), {control->token(), observe});
        print(Json{{"snapshot", snapshot ? Json(*snapshot) : Json{}}});
      }
    } else if (sub == "archives") {
      const auto kind = args.option("--kind"), after = args.option("--after");
      const auto limit = integer(args.option("--limit").value_or("100"));
      require(limit >= 1 && limit <= 1000, "native archive page limit must be in 1..1000");
      const bool verify = args.flag("--verify");
      args.count(0);
      print(store.native_archives(kind, after, static_cast<std::size_t>(limit), verify,
                                  {control->token(), observe}));
    } else if (sub == "archive-show") {
      const bool verify = args.flag("--verify");
      args.count(1);
      print(store.native_archive(args.values[0], verify, {control->token(), observe}));
    } else if (sub == "squash-plan" || sub == "squash") {
      const bool planning = sub == "squash-plan";
      const auto branch = selected_branch();
      const bool verify = planning && args.flag("--verify");
      const auto expected = planning ? std::optional<std::string>{} : args.option("--expect");
      const auto message = planning ? std::optional<std::string>{} : args.option("--message", "-m");
      require(planning || message.has_value(), "--message is required");
      args.count(1);
      const auto count = integer(args.values[0]);
      require(count >= 2 && count <= 100000, "native squash count must be in 2..100000");
      if (planning) {
        const auto plan =
            store.plan_squash(branch, static_cast<std::size_t>(count), verify, {control->token(), observe});
        print(plan);
        return plan["ready"].get<bool>() ? 0 : 1;
      }
      const auto tip = expected ? *expected : resolve("heads/" + branch);
      print(store.squash_commits(branch, tip, static_cast<std::size_t>(count), *message,
                                 {control->token(), observe}));
    } else if (sub == "status") {
      const auto combined = args.flag("--combined");
      const auto indexed = args.flag("--indexed");
      const auto directory = workspace();
      const auto branch = args.option("--branch");
      auto revision = args.option("--revision");
      args.count(0);
      require(!(branch && revision), "choose either --branch or --revision");
      require(!(combined && revision), "combined native status cannot select an explicit revision");
      if (revision)
        *revision = resolve(*revision);
      if (combined) {
        const auto selected = branch ? *branch : store.native_head()["branch"].get<std::string>();
        print(indexed ? indexed_native_workspace_status(store, directory, selected, {control->token(), observe})
                      : store.workspace_status(directory, selected, {control->token(), observe}));
        return 0;
      }
      if (!revision) {
        const auto name = "heads/" + (branch ? *branch : store.native_head()["branch"].get<std::string>());
        validate_ref(name);
        const auto all = store.refs();
        if (const auto found = all.find(name); found != all.end()) {
          require(found->second.kind == Kind::revision, "branch must point to a revision");
          revision = found->second.id;
        }
      }
      print(indexed ? indexed_native_status(store, directory, revision, {control->token(), observe})
                    : store.status_directory(directory, revision, {control->token(), observe}));
    } else if (sub == "index-clear") {
      args.count(0);
      print(clear_native_working_index(store, {control->token(), observe}));
    } else if (sub == "checkout") {
      const auto directory = workspace();
      const auto branch = selected_branch();
      args.count(1);
      print(
          store.checkout_directory(directory, branch, resolve(args.values[0]), {control->token(), observe}));
    } else if (sub == "checkout-status") {
      const auto id = args.option("--id");
      args.count(0);
      print(store.checkout_status(id));
    } else if (sub == "checkout-recover") {
      const auto abort = args.flag("--abort");
      args.count(0);
      print(store.recover_checkout(abort, {control->token(), observe}));
    } else if (sub == "checkout-plan") {
      const auto directory = workspace();
      const auto branch = selected_branch();
      const auto verify = args.flag("--verify");
      args.count(1);
      const auto plan = store.plan_checkout(directory, branch, resolve(args.values[0]), verify,
                                            {control->token(), observe});
      print(plan);
      return plan["ready"].get<bool>() ? 0 : 1;
    } else if (sub == "head") {
      args.count(0);
      print(store.native_head());
    } else if (sub == "switch" || sub == "switch-plan") {
      const auto directory = workspace();
      const auto verify = sub == "switch-plan" && args.flag("--verify");
      args.count(1);
      if (sub == "switch")
        print(store.switch_directory(directory, args.values[0], {control->token(), observe}));
      else {
        const auto plan = store.plan_switch(directory, args.values[0], verify, {control->token(), observe});
        print(plan);
        return plan["ready"].get<bool>() ? 0 : 1;
      }
    } else if (sub == "merge-text") {
      const auto limits = text_diff_options(args);
      const bool raw = args.flag("--text");
      args.count(4);
      const auto report =
          store.merge_revision_file(resolve(args.values[0]), resolve(args.values[1]), resolve(args.values[2]),
                                    args.values[3], limits, {control->token(), observe});
      const auto ready = report["ready"].get<bool>();
      if (raw) {
        require(ready, "native text merge is not ready; inspect native merge-text without --text");
        throw_if_cancelled(control->token());
        const auto &content = report["merge"]["content"].get_ref<const std::string &>();
        std::cout.write(content.data(), static_cast<std::streamsize>(content.size()));
        require(std::cout.good(), "output write failed");
      } else {
        print(report);
      }
      return ready ? 0 : 1;
    } else if (sub == "diff-text") {
      const auto limits = text_diff_options(args);
      const bool unified = args.flag("--unified");
      const bool staged = args.flag("--staged");
      const bool working = args.flag("--working");
      require(!staged || !working, "choose either --staged or --working");
      Json report;
      if (working) {
        const auto directory = workspace();
        const auto revision = args.option("--revision");
        if (revision) {
          args.count(1);
          report = store.diff_working_file(directory, resolve(*revision), args.values[0], limits,
                                           {control->token(), observe});
        } else {
          const auto branch = selected_branch();
          args.count(1);
          report = store.diff_unstaged_file(directory, branch, args.values[0], limits,
                                            {control->token(), observe});
        }
      } else if (staged) {
        const auto branch = selected_branch();
        args.count(1);
        report = store.diff_staged_file(branch, args.values[0], limits, {control->token(), observe});
      } else {
        args.count(3);
        report = store.diff_revision_file(resolve(args.values[0]), resolve(args.values[1]), args.values[2],
                                          limits, {control->token(), observe});
      }
      if (unified)
        std::cout << cli::unified_text(report);
      else
        print(report);
    } else if (sub == "diff") {
      const auto renames = args.flag("--renames");
      args.count(2);
      const auto source = resolve(args.values[0]), target = resolve(args.values[1]);
      print(renames ? native_diff_renames(store, source, target, {control->token(), observe})
                    : store.compare_revisions(source, target, {control->token(), observe}));
    } else if (sub == "log-file") {
      NativeFileHistoryOptions limits;
      const auto limit = integer(args.option("--limit").value_or("100"));
      const auto visited = integer(args.option("--max-revisions").value_or("1000"));
      require(limit >= 1 && limit <= 1000, "native file history page limit must be in 1..1000");
      require(visited >= 1 && visited <= 10000, "native file history traversal limit must be in 1..10000");
      limits.limit = static_cast<std::size_t>(limit);
      limits.max_revisions = static_cast<std::size_t>(visited);
      limits.follow_renames = !args.flag("--no-follow");
      args.count(2);
      print(native_file_history(store, resolve(args.values[0]), args.values[1], limits,
                                {control->token(), observe}));
    } else if (sub == "branch") {
      args.count(2);
      (void)store.create_native_branch(args.values[0], resolve(args.values[1]), {control->token(), observe});
    } else if (sub == "branches") {
      const auto after = args.option("--after");
      const auto limit = integer(args.option("--limit").value_or("100"));
      require(limit >= 1 && limit <= 1000, "native branch page limit must be in 1..1000");
      const bool verify = args.flag("--verify");
      args.count(0);
      print(
          store.native_branches(after, static_cast<std::size_t>(limit), verify, {control->token(), observe}));
    } else if (sub == "branch-delete-plan" || sub == "branch-delete") {
      const bool planning = sub == "branch-delete-plan";
      const bool archive_index = args.flag("--archive-index");
      const bool verify = planning && args.flag("--verify");
      const auto expected = planning ? std::optional<std::string>{} : args.option("--expect");
      auto index = planning ? std::optional<std::string>{} : args.option("--expect-index");
      require(planning || expected.has_value(), "--expect is required for native branch deletion");
      require(!index || archive_index, "--expect-index requires --archive-index");
      args.count(1);
      if (planning) {
        const auto plan = store.plan_delete_native_branch(args.values[0], archive_index, verify,
                                                          {control->token(), observe});
        print(plan);
        return plan["ready"].get<bool>() ? 0 : 1;
      }
      if (archive_index && !index) {
        const auto all = store.refs();
        if (const auto found = all.find("staging/" + args.values[0]); found != all.end()) {
          require(found->second.kind == Kind::revision, "invalid native staging snapshot");
          index = found->second.id;
        }
      }
      print(store.delete_native_branch(args.values[0], *expected, index, {control->token(), observe}));
    } else if (sub == "branch-archives") {
      const auto after = args.option("--after");
      const auto limit = integer(args.option("--limit").value_or("100"));
      require(limit >= 1 && limit <= 1000, "native branch page limit must be in 1..1000");
      const bool verify = args.flag("--verify");
      args.count(0);
      print(store.native_branch_archives(after, static_cast<std::size_t>(limit), verify,
                                         {control->token(), observe}));
    } else if (sub == "branch-archive") {
      const bool verify = args.flag("--verify");
      args.count(1);
      print(store.native_branch_archive(args.values[0], verify, {control->token(), observe}));
    } else if (sub == "branch-restore-plan" || sub == "branch-restore") {
      const bool planning = sub == "branch-restore-plan";
      const auto name = args.option("--as");
      const bool verify = planning && args.flag("--verify");
      args.count(1);
      if (planning) {
        const auto plan =
            store.plan_restore_native_branch(args.values[0], name, verify, {control->token(), observe});
        print(plan);
        return plan["ready"].get<bool>() ? 0 : 1;
      }
      print(store.restore_native_branch(args.values[0], name, {control->token(), observe}));
    } else if (sub == "tag") {
      const auto message = args.option("--message", "-m");
      args.count(args.values.size() -
                 (std::find(args.values.begin(), args.values.end(), "--") != args.values.end()));
      require(args.values.size() == 1 || args.values.size() == 2,
              "unexpected or missing arguments; use --help");
      const auto revision = resolve(args.values.size() == 2 ? args.values[1] : "HEAD");
      print(store.create_native_tag(args.values[0], revision, message, {control->token(), observe}));
    } else if (sub == "tags") {
      const auto after = args.option("--after");
      const auto limit = integer(args.option("--limit").value_or("100"));
      require(limit >= 1 && limit <= 1000, "native tag page limit must be in 1..1000");
      args.count(0);
      print(store.native_tags(after, static_cast<std::size_t>(limit), {control->token(), observe}));
    } else if (sub == "tag-show" || sub == "tag-archive") {
      const auto verify = args.flag("--verify");
      args.count(1);
      print(sub == "tag-show"
                ? store.native_tag(args.values[0], verify, {control->token(), observe})
                : store.native_tag_archive(args.values[0], verify, {control->token(), observe}));
    } else if (sub == "tag-delete") {
      const auto expected = args.option("--expect");
      args.count(1);
      require(expected.has_value(), "--expect is required for native tag deletion");
      print(store.delete_native_tag(args.values[0], *expected, {control->token(), observe}));
    } else if (sub == "tag-restore") {
      args.count(1);
      print(store.restore_native_tag(args.values[0], {control->token(), observe}));
    } else if (sub == "cherry-pick-plan" || sub == "revert-plan" || sub == "cherry-pick" || sub == "revert") {
      const bool planning = sub.ends_with("-plan");
      const auto directory = workspace();
      const auto branch = selected_branch();
      const auto verify = planning && args.flag("--verify");
      const auto expected = planning ? std::optional<std::string>{} : args.option("--expect");
      const auto message = planning ? std::optional<std::string>{} : args.option("--message", "-m");
      require(planning || message.has_value(), "--message is required");
      NativeReplayOptions settings;
      settings.resolutions = resolutions_input();
      if (args.flag("--text-merge"))
        settings.text_merge = text_diff_options(args);
      if (const auto value = args.option("--mainline")) {
        const auto n = integer(*value);
        require(n >= 1 && n <= UINT32_MAX, "native replay mainline must be in 1..4294967295");
        settings.mainline = static_cast<std::uint32_t>(n);
      }
      args.count(1);
      const auto kind = sub.starts_with("revert") ? NativeReplayKind::revert : NativeReplayKind::cherry_pick;
      const auto source = resolve(args.values[0]);
      if (planning) {
        auto plan = store.plan_replay_with_options(directory, branch, source, kind, settings, verify,
                                                   {control->token(), observe});
        const bool ready = plan["ready"].get<bool>();
        print(plan);
        return ready ? 0 : 1;
      }
      const auto tip = expected ? *expected : resolve("heads/" + branch);
      print(store.replay_directory_with_options(directory, branch, tip, source, kind, *message, settings,
                                                {control->token(), observe}));
    } else if (sub == "merge-plan" || sub == "merge") {
      const bool planning = sub == "merge-plan";
      const auto directory = workspace();
      const auto branch = selected_branch();
      const auto verify = planning && args.flag("--verify");
      const auto expected = planning ? std::optional<std::string>{} : args.option("--expect");
      const auto message = planning ? std::optional<std::string>{} : args.option("--message", "-m");
      NativeMergeOptions settings;
      settings.resolutions = resolutions_input();
      if (args.flag("--text-merge"))
        settings.text_merge = text_diff_options(args);
      settings.allow_unrelated_histories = args.flag("--allow-unrelated-histories");
      settings.no_fast_forward = args.flag("--no-ff");
      settings.fast_forward_only = args.flag("--ff-only");
      const auto base = args.option("--base");
      const auto revisions = integer(args.option("--max-revisions").value_or("100000"));
      const auto links = integer(args.option("--max-links").value_or("1000000"));
      require(revisions >= 1 && revisions <= 1000000, "native graph revision limit must be in 1..1000000");
      require(links <= 10000000, "native graph parent link limit must be in 0..10000000");
      settings.limits = {static_cast<std::size_t>(revisions), static_cast<std::size_t>(links)};
      args.count(1);
      if (base)
        settings.base = resolve(*base);
      const auto source = resolve(args.values[0]);
      if (planning) {
        const auto plan =
            store.plan_merge(directory, branch, source, settings, verify, {control->token(), observe});
        print(plan);
        return plan["ready"].get<bool>() ? 0 : 1;
      }
      const auto tip = expected ? *expected : resolve("heads/" + branch);
      print(store.merge_directory(directory, branch, tip, source, message.value_or("Merge " + source),
                                  settings, {control->token(), observe}));
    } else if (sub == "rebase-plan" || sub == "rebase-todo" || sub == "rebase") {
      const bool planning = sub != "rebase", exporting = sub == "rebase-todo";
      const auto directory = workspace();
      const auto branch = selected_branch();
      const auto onto = args.option("--onto");
      require(onto.has_value(), "native rebase requires --onto");
      const auto limit = integer(args.option("--max-commits").value_or("1000"));
      require(limit >= 1 && limit <= 100000, "native rebase commit limit must be in 1..100000");
      NativeRebaseOptions settings;
      if (args.flag("--text-merge"))
        settings.text_merge = text_diff_options(args);
      std::optional<NativeRebaseResolutionSet> resolutions;
      if (const auto file = args.option("--resolutions")) {
        throw_if_cancelled(control->token());
        const auto data = read_bytes(path_from_utf8(*file), max_object_size);
        resolutions = NativeRebaseResolutionSet::decode(data);
      }
      const bool verify = planning && args.flag("--verify");
      const auto expected = planning ? std::optional<std::string>{} : args.option("--expect");
      const auto todo_file = args.option("--todo");
      const bool interactive = args.flag("--interactive");
      require(!todo_file || !interactive, "choose a rebase todo file or interactive input");
      require(!exporting || (!todo_file && !interactive), "native rebase todo export cannot take a sequence");
      require(!interactive || !json_progress, "interactive rebase cannot use JSON progress");
      args.count(1);
      settings.max_commits = static_cast<std::size_t>(limit);
      settings.resolutions = std::move(resolutions);
      const auto base = resolve(args.values[0]), target = resolve(*onto);
      if (todo_file)
        settings.todo = NativeRebaseTodo::decode(read_bytes(path_from_utf8(*todo_file), max_object_size));
      if (exporting || interactive) {
        const auto original =
            store.plan_rebase(directory, branch, base, target, settings, verify, {control->token(), observe});
        require(!original["todo"].is_null(), "native rebase todo requires a linear range");
        if (exporting) {
          print(original["todo"]);
          return 0;
        }
        settings.todo = interactive_rebase_todo(store, NativeRebaseTodo::parse(original["todo"]), language,
                                                control->token());
      }
      if (planning) {
        const auto plan =
            store.plan_rebase(directory, branch, base, target, settings, verify, {control->token(), observe});
        print(plan);
        return plan["ready"].get<bool>() ? 0 : 1;
      }
      print(store.rebase_directory(directory, branch, expected ? *expected : resolve("heads/" + branch), base,
                                   target, settings, {control->token(), observe}));
    } else if (sub == "ancestry") {
      const auto revisions = integer(args.option("--max-revisions").value_or("100000"));
      const auto links = integer(args.option("--max-links").value_or("1000000"));
      require(revisions >= 1 && revisions <= 1000000, "native graph revision limit must be in 1..1000000");
      require(links <= 10000000, "native graph parent link limit must be in 0..10000000");
      args.count(2);
      print(store.native_ancestry(resolve(args.values[0]), resolve(args.values[1]),
                                  {static_cast<std::size_t>(revisions), static_cast<std::size_t>(links)},
                                  {control->token(), observe}));
    } else if (sub == "log") {
      auto branch = selected_branch();
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
      store.export_revision(resolve(args.values[0]), path_from_utf8(args.values[1]),
                            {control->token(), observe});
    } else if (sub == "detach-history") {
      auto branch = selected_branch();
      auto message = args.option("--message", "-m");
      args.count(0);
      require(message.has_value(), "--message is required");
      std::cout << store.detach_history(branch, *message) << '\n';
    } else
      throw Error("unknown native command");
  } else if (command == "git") {
    if (sub == "retention") {
      auto apply = args.flag("--apply");
      args.count(0);
      auto result = git::reconcile_retention(store, git_directory, apply, {control->token(), observe});
      auto ready = result["ready"].get<bool>();
      print(result);
      return ready ? 0 : 1;
    } else if (sub == "migrate-lfs" || sub == "migrate-lfs-history") {
      auto branch = args.option("--branch");
      require(branch.has_value(), "--branch is required for Git LFS migration");
      auto cache = args.option("--lfs-objects");
      auto objects =
          git::lfs_objects(git_directory, cache ? std::optional{path_from_utf8(*cache)} : std::nullopt);
      auto options = ingestion_options(args, store);
      options.stop = control->token();
      options.progress = observe;
      git::LfsHistoryOptions history;
      if (sub == "migrate-lfs-history") {
        history.strip_signatures = args.flag("--strip-signatures");
        if (auto value = args.option("--max-commits")) {
          const auto limit = integer(*value);
          require(limit >= 1 && limit <= 100000, "Git LFS history commit limit must be in 1..100000");
          history.max_commits = static_cast<std::size_t>(limit);
        }
      }
      args.count(1);
      print(sub == "migrate-lfs"
                ? git::migrate_lfs(store, git_directory, args.values[0], objects, *branch, options)
                : git::migrate_lfs_history(store, git_directory, args.values[0], objects, *branch, history,
                                           options));
    } else if (sub == "pre-push") {
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
  } else if (command == "backup") {
    const OperationOptions operation{control->token(), observe};
    std::optional<Key> secondary;
    if (const auto encoded = environment("SYRNCK_BACKUP_KEY")) {
      const auto value = unhex(*encoded);
      require(value.size() == 32, "SYRNCK_BACKUP_KEY must encode exactly 32 bytes");
      secondary.emplace();
      std::copy(value.begin(), value.end(), secondary->begin());
    }
    if (sub == "verify") {
      args.count(0);
      print(verify_backup(store, operation));
    } else if (sub == "create" || sub == "restore") {
      args.count(1);
      if (!secondary && store.encrypted())
        secondary = read_key();
      const auto destination = path_from_utf8(args.values[0]);
      print(sub == "create" ? create_backup(store, destination, secondary, operation)
                            : restore_backup(store, destination, secondary, operation));
    } else if (sub == "checkout-retire-plan" || sub == "checkout-retire") {
      const auto from = args.option("--from");
      const auto expected = args.option("--expect-plan");
      const bool sources = args.flag("--include-source-archives");
      require(from.has_value(), "checkout retirement requires --from BACKUP");
      require(sub == "checkout-retire-plan" || expected.has_value(),
              "checkout retirement requires --expect-plan");
      const auto ids = args.paths();
      Store backup(path_from_utf8(*from), secondary);
      print(sub == "checkout-retire-plan"
                ? plan_checkout_retirement(store, backup, ids, sources, operation)
                : retire_checkouts(store, backup, ids, *expected, sources, operation));
    } else if (sub == "retire-plan" || sub == "retire") {
      const auto from = args.option("--from");
      const auto expected = args.option("--expect-plan");
      require(from.has_value(), "archive retirement requires --from BACKUP");
      require(sub == "retire-plan" || expected.has_value(), "archive retirement requires --expect-plan");
      const auto names = args.paths();
      Store backup(path_from_utf8(*from), secondary);
      print(sub == "retire-plan" ? plan_archive_retirement(store, backup, names, operation)
                                 : retire_archives(store, backup, names, *expected, operation));
    } else if (sub == "repair-copy" || sub == "repair-copy-plan") {
      args.count(sub == "repair-copy" ? 2 : 1);
      Store backup(path_from_utf8(args.values[0]), secondary);
      if (sub == "repair-copy-plan")
        print(plan_repaired_copy(store, backup, operation));
      else {
        auto destination_key = read_key();
        if (!destination_key && backup.encrypted())
          destination_key = secondary;
        if (const auto encoded = environment("SYRNCK_RECOVERY_KEY")) {
          const auto value = unhex(*encoded);
          require(value.size() == 32, "SYRNCK_RECOVERY_KEY must encode exactly 32 bytes");
          destination_key.emplace();
          std::copy(value.begin(), value.end(), destination_key->begin());
        }
        print(
            create_repaired_copy(store, backup, path_from_utf8(args.values[1]), destination_key, operation));
      }
    } else if (sub == "repair") {
      const auto apply = args.flag("--apply");
      args.count(1);
      Store backup(path_from_utf8(args.values[0]), secondary);
      const auto result = repair_from_backup(store, backup, apply, operation);
      print(result);
      require(std::cout.good(), "output write failed");
      return result["ready"].get<bool>() ? 0 : 1;
    } else
      throw Error("unknown backup command");
  } else if (command == "locks") {
    if (const auto url = args.option("--remote")) {
      require(token.has_value(), "set SYRNCK_TOKEN");
      net::Remote remote(*url, *token, ca_path);
      const OperationOptions operation{control->token(), observe};
      Json request = Json::object();
      if (sub == "identity") {
        args.count(0);
        print(remote.identity(operation));
        return 0;
      } else if (sub == "list") {
        const auto after = args.option("--after");
        const auto limit = integer(args.option("--limit").value_or("100"));
        args.count(0);
        request = Json{{"after", after ? Json(*after) : Json{}}, {"limit", limit}};
      } else if (sub == "prune") {
        const auto remove = args.flag("--delete");
        args.count(0);
        request = Json{{"delete", remove}};
      } else {
        const auto generation = args.option("--fence");
        const auto seconds = integer(args.option("--seconds").value_or("3600"));
        const auto paths = args.paths();
        if (sub == "acquire" || sub == "acquire-plan")
          request = Json{{"paths", paths}, {"seconds", seconds}};
        else {
          require(generation.has_value(), "asset lock command requires --fence");
          request["claims"] = Json::array();
          for (const auto &path : paths)
            request["claims"].push_back(Json{{"path", path}, {"fence", integer(*generation)}});
          if (sub == "renew")
            request["seconds"] = seconds;
        }
      }
      const auto result = remote.locks(sub, request, operation);
      print(result);
      require(std::cout.good(), "output write failed");
      return sub == "acquire-plan" && !result.at("ready").get<bool>() ? 1 : 0;
    }
    AssetLocks locks(store);
    const OperationOptions operation{control->token(), observe};
    if (sub == "list") {
      const auto after = args.option("--after");
      const auto limit = integer(args.option("--limit").value_or("100"));
      require(limit >= 1 && limit <= 1000, "asset lock page limit must be in 1..1000");
      args.count(0);
      print(locks.list(after, static_cast<std::size_t>(limit), operation));
    } else if (sub == "prune") {
      const bool remove = args.flag("--delete");
      args.count(0);
      print(locks.prune(remove, operation));
    } else if (sub == "acquire-plan" || sub == "acquire" || sub == "renew" || sub == "release") {
      const auto owner = args.option("--owner");
      require(owner.has_value(), "asset lock command requires --owner");
      const auto seconds = sub == "release" ? 3600 : integer(args.option("--seconds").value_or("3600"));
      const auto fence =
          sub == "renew" || sub == "release" ? args.option("--fence") : std::optional<std::string>{};
      require((sub != "renew" && sub != "release") || fence.has_value(),
              "asset lock command requires --fence");
      const auto paths = args.paths();
      if (sub == "acquire-plan") {
        const auto plan = locks.acquire_plan(*owner, paths, seconds, operation);
        print(plan);
        return plan["ready"].get<bool>() ? 0 : 1;
      }
      if (sub == "acquire")
        print(locks.acquire(*owner, paths, seconds, operation));
      else {
        const auto generation = integer(*fence);
        std::vector<AssetLockClaim> claims;
        for (const auto &path : paths)
          claims.push_back({path, generation});
        print(sub == "renew" ? locks.renew(*owner, claims, seconds, operation)
                             : locks.release(*owner, claims, operation));
      }
    } else
      throw Error("unknown asset lock command");
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
    if (const auto path = args.option("--access"))
      limits.access = parse_json(read_bytes(path_from_utf8(*path), 1024 * 1024));
    limits.object_compression = !args.flag("--no-compression");
    limits.require_upload_lease = args.flag("--require-lease");
    if (auto seconds = args.option("--lease-seconds"))
      limits.upload_lease_seconds = integer(*seconds);
    if (auto seconds = args.option("--read-lease-seconds"))
      limits.read_lease_seconds = integer(*seconds);
    auto listen = args.option("--listen").value_or("127.0.0.1:7437");
    args.count(0);
    require(limits.access.has_value() || token.has_value(), "set SYRNCK_TOKEN");
    net::serve(store, token.value_or(""), listen, language, limits);
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
  } catch (const git::LfsPublicationUncertain &error) {
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
  try {
    for (int i = 1; i < argc; ++i)
      args.values.push_back(utf8_cli_text(argv[i]));
  } catch (const std::exception &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
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
