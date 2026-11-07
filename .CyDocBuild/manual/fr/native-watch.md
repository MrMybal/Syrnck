---
id: native-watch
title: Surveillance résidente du workspace
description: Guide : Surveillance résidente du workspace.
section: Versionnage
order: 410
parent: native-history
sourceRevision: 1
---

# Observation résidente du workspace {#resident-native-workspace-observation}

`native watch` observe l’état courant : notifications récursives Windows, inotify récursif Linux, polling ailleurs ou avec `--polling`, en cas d’indisponibilité/limite native.

Une ligne JSON compacte est émise puis flushée par frame. Clés/types/raisons restent anglais ; erreurs/progression stderr sont bilingues. Sans durée/limite d’updates, arrêt par interruption. Il observe sans commit, modification de fichiers, publication, transfert, verrou d’asset ni résolution de concurrence. C’est une fondation pour un futur agent résident. Les [sessions C/Go](doc:workspace-abi) exposent watcher/inventaire avec lots retenus et Store appartenant à l’appelant, sans journal CLI.

```text
syrnck native watch
syrnck native watch --combined --branch main --progress
syrnck native watch --directory PROJECT --max-updates 10
syrnck native watch --polling --interval-ms 1000 --duration-ms 10000
syrnck native watch --audit-ms 60000 --debounce-ms 150 --max-paths 2048
syrnck native watch --journal --combined
syrnck native watch-journal
syrnck native watch-journal --clear
```


## Cycle des événements et états {#status-and-event-lifecycle}

Le watcher est armé avant la première requête. Le démarrage efface le cache de hash consultatif et lit tous les fichiers sélectionnés, couvrant les edits hors ligne. Les notifications sont coalescées sur un intervalle fixe borné puis remplacent les sous-arbres concernés de l’inventaire complet en mémoire. Les règles ignore ordinaires s’appliquent. Les fichiers concernés sont intégralement lus même à taille/dates identiques ; les autres sous-arbres ne sont ni énumérés ni lus. Les observations ciblées ne réécrivent pas le cache persisté.

Créations, suppressions, renommages et transitions remplacent leurs préfixes complets. Un ancêtre absent/ignoré/fichier élargit la portée pour retirer les entrées périmées ; `asset` et `assets` sont distincts. Les hints imbriqués fusionnent. Un `.syrnckignore` imbriqué resélectionne son sous-arbre, avec règles enfants ; celui de racine impose le scan complet. Collisions portables fichier/dossier/casse et borne de métadonnées v1 sont contrôlées sur l’union retenue+nouvelle avant hash. Sélection ambiguë, portée protégée ou budget dépassé impose un scan complet ; une erreur persistante reste une erreur. Liens/reparse pour ignore files sont refusés ; un pattern invalide donne une erreur de scan récupérable.

Les noms d’événements sont des indices d’état courant, pas un log transactionnel ni une paire de renommage garantie. `--combined` sépare index et travail, index périmé inclus. Branche/révision suivent status ; combined refuse une révision explicite.

L’attente native ne parcourt ni dossiers ni Store. Le Store est ouvert seulement pendant la requête puis fermé avant sortie/attente/coalescence pour laisser les autres commandes prendre le verrou. Cache/objets/.git sont exclus de la boucle ; les vrais `refs.json`/`config.json` restent visibles. Windows vérifie l’identité des alias de Store ; `Vault` distinct de `vault` sur FS sensible à la casse reste observé.

Seuls un état sémantique changé, un scan complet terminé ou une nouvelle erreur émettent. Compteurs de hash/réutilisation et générations seuls restent silencieux. Le polling énumère à chaque intervalle mais n’émet pas les résultats identiques.

Le champ `status` reprend les rapports ordinaires/combinés. `observation` donne `mode: full|indexed|targeted`, `full_scan`, `inspected_files`, `retained_files`; les ciblés ajoutent `scanned_scopes` triés après coalescence/ancêtres. Une suppression peut lire zéro fichier/octet. Seuls les scans énumérés incluent les métriques `working_index`; l’inventaire de session reste complet même pour les fichiers non cachables. Le fallback marque aussi la frame `full_scan`. Comparer les maps et lire les métadonnées reste proportionnel, sans status constant.

`sequence` croît dans le processus, erreurs comprises. Backends : `windows`, `inotify`, `polling`. Raisons : `initial` première réussite ; `filesystem` notification avec changement ; `polling` changement observé ; `audit` lecture complète périodique ; `rescan` topologie/enregistrement ; `overflow` perte/budget ; `recovered` scan complet après indisponibilité.

```json
{"version":1,"sequence":1,"type":"status","reason":"initial","backend":"windows","full_scan":true,"overflow":false,"paths":[],"status":{"version":1,"base":null,"clean":true,"tracked_files":0,"workspace_files":0,"hashed_bytes":0,"counts":{"untracked":0,"removed":0,"modified":0,"unchanged":0},"changes":[],"working_index":{"version":1,"mode":"stat-v1","previous":"missing","hashed_files":0,"reused_files":0,"supported_files":0,"cached_files":0,"truncated":false,"object":"INDEX_OBJECT_ID"}}}
```


## Pertes d’événements, audits et indisponibilité {#lost-events-audits-and-unavailable-observations}

Overflow, enregistrement incomplet/malformé, budget de hints ou panne native abandonnent les listes partielles et exigent un scan complet sans cache. Noms non UTF-8 : hints abandonnés, scan complet, affichage avec remplacement des octets invalides pour garder un JSON valide.

Linux arme les nouveaux dossiers/imports avant d’énumérer leurs enfants sur la même instance inotify, puis observe leur sous-arbre entier ; les écritures ultérieures restent en queue. Enregistrements existants conservés, additions coalescées et budgets persistants appliqués. Suppression, sortie, self-move, watch ignoré ou démontage reconstruisent l’instance et imposent un scan complet ; aucun renommage des chemins par cookie. Descripteurs inconnus/alias, trames invalides, overflow et extension ratée/dépassée exigent récupération. Instance partielle fermée ; une annulation invalide la continuité. Une indisponibilité/quota peut revenir au polling.

Les audits complets périodiques ont un calendrier indépendant des notifications/polling : défaut cinq minutes, réglable. Ils couvrent notamment FS distants, notifications manquées et écritures mappées.

Store occupé, checkout actif ou scan raté : frame `type: error`, `code: scan_failed`, message traduit, `status: null`. L’ancien succès n’est pas présenté comme courant. Erreurs consécutives identiques supprimées ; reprise au moins une seconde plus tard sans verrou conservé. Le retour au succès impose un scan complet. Un changement permanent d’identité de racine demande un redémarrage ; un handle de dossier retient cette identité sans interdire les edits ni verrouiller le Store.

L’observation ne fournit ni snapshot atomique/verrouillé, ni versions intermédiaires, ni autorisation de sauter les validations des mutations.

## Journal de checkpoint optionnel {#optional-persistent-checkpoint-journal}

`--journal` écrit le lot coalescé avant traitement, puis `observed` avant sortie réussie ou `unavailable` après erreur si stockage accessible. C’est un enregistrement borné remplacé, pas un historique rejouable : digests workspace/sélection, session aléatoire, génération, ancienne session/état, backend, hints bornés, digest sémantique, base, propreté et nombre de fichiers, sans contenu ni rapport complet.

Les frames ajoutent `journal: {session,generation,durable,previous}`. `durable: true` signifie sauvegarde avant stdout, sans accusé du consommateur. `sequence` reste local ; un arrêt entre publication et livraison exige de rétablir l’état à la reconnexion. Un Store indisponible peut donner `durable: false`. Un journal corrompu est préservé et bloque ce mode jusqu’à effacement explicite ; le mode ordinaire reste utilisable.

`native watch-journal` lit le dernier record ou `state: absent`. `observed` décrit une ancienne requête, pas un watcher vivant ni un état encore courant. `pending` peut subsister après arrêt. Tout démarrage crée une nouvelle session et lit tous les octets, même après un checkpoint valide ; le premier record conserve l’ancienne session/état.

`--clear` retire explicitement le journal local, même malformé dans la borne de taille, sans modifier objets/références/index/workspace. Le mode journal détient `watch.lock`, interdisant second propriétaire/clear concurrent. Le Store reste libre entre requêtes et le checkpoint consultable ; sans journal, pas de verrou de session.

Fichier autonome : `SYW1`, digest de chunk sur 64 octets et enveloppe Store compressée, chiffrée par la clé AEAD existante si nécessaire, noms inclus. Digest et mode restent visibles. Bornes : clair 2 Mio, fichier 3 Mio, 65536 chemins, budget de noms 1 Mio. Écriture temporaire, flush et remplacement atomique durable, publication après callbacks. Aucun objet/racine temporaire ; GC/compact ne l’effacent pas, backups logiques l’omettent.

Attente native et polling inchangé n’écrivent rien. Un changement polling est checkpointé avant sortie ; audit écrit pending puis observed. Versions causales, outbox et publication réseau exactly-once restent distinctes.

## Limites et ressources {#limits-and-resource-use}

Racines workspace/Store doivent exister, être lisibles et identifiables.

| Option | Défaut | Valeurs |
| --- | --- | --- |
| `--interval-ms` | 500 | 20..60000, polling et réveil natif |
| `--debounce-ms` | 75 | 0..5000 |
| `--audit-ms` | 300000 | 1000..3600000 |
| `--duration-ms` | 0 | illimité, sinon 100..86400000 |
| `--max-updates` | 0 | illimité, sinon 1..1000000 succès |
| `--max-paths` | 4096 | 1..65536 hints uniques |

Durée contrôlée entre observations, sans timeout de syscall ; erreurs hors quota succès. À expiration :1 si dernier état indisponible, sinon 0. Interruption selon le CLI ordinaire.

Windows : tampon 64 Kio aligné DWORD et copie bornée. Linux : lecture 64 Kio,65536 enregistrements,128 niveaux,16 Mio de noms, avec quotas OS potentiellement inférieurs. Hints natifs et lot CLI :1 Mio de noms chacun. Sets/maps/paths/snapshots/rapports/queues noyau/allocations s’ajoutent ; aucun budget total RAM. Un consommateur bloqué peut faire déborder les événements et imposer un scan.

Observation ciblée C++ :65536 hints/1 Mio ; noms de dossiers/ancêtres et ignore bytes 16 Mio par portée, noms sélectionnés 16 Mio conservateurs. Dépassement : scan complet. Scan complet et union retenue/remplacement appliquent la borne de métadonnées v1, sans plafond RAM/syscalls global.

## API C++ et validation {#c-api-and-validation}

API à appelant unique ; un autre thread peut demander l’annulation. `invalidate()` force le prochain scan immédiatement sans jeter les notifications. `wait` accepte 0..60000 ms,0 immédiat ; attente native/polling vérifie l’annulation au plus toutes les 50 ms hors latence FS/syscall. Les [sessions C/Go](doc:workspace-abi) exposent observation et lots, sans journal CLI.

`WorkspaceObservation(project,store_path)` fournit `status(store,revision,hints,operation)` et `workspace_status(store,branch,hints,operation)`. Armer d’abord le watcher puis transmettre chaque lot sans trou ; nouvelle observation, invalidate, erreur ou audit abandonnent les hypothèses incrémentales. Le polling peut utiliser le stat-cache entre audits d’octets. Fermer/réouvrir le Store ne perd pas l’inventaire, ni collecter les objets de cache non retenus. Les mutations revalident indépendamment.

```cpp
#include <syrnck/watcher.hpp>
syrnck::WorkspaceWatcher watcher(project, store_path, {}, stop);
// Perform the caller's initial complete observation after construction.
auto hints = watcher.wait(std::chrono::milliseconds(500), stop);
if (hints.full_scan) {
    // Reestablish a complete observation before trusting incremental hints.
}
watcher.invalidate(); // The caller lost its own observation state.
```


## Limites d’observation {#synthetic-performance-measurement}

Quand la continuité est préservée, fichiers/sous-arbres utilisent l’observation ciblée. Linux étend les créations/imports ; suppressions/mouvements de dossiers déjà suivis reconstruisent l’instance et imposent une lecture complète. La validation réelle Linux/macOS reste nécessaire. Historique rejouable, causalité, tombstones, ordonnanceur de transfert, packaging service OS et interopérabilité daemon/dossier Syncthing restent à développer. Voir les [ressources](doc:performance) et les références officielles [Windows](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw) et [inotify](https://www.man7.org/linux/man-pages/man7/inotify.7.html).
