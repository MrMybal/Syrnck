---
id: asset-locks
title: Verrous temporaires d’assets
description: Guide : Verrous temporaires d’assets.
section: Stockage
order: 30
parent: storage
sourceRevision: 1
---

# Verrous temporaires d’assets {#repository-local-asset-leases}

`syrnck::AssetLocks` fournit une table de baux atomiques dans un Store existant : acquisition groupée, renouvellement, libération et purge des expirations. Les [droits serveur](doc:server-access) la relient aux identités HTTP authentifiées et aux contrôles de publication des branches. L’appelant C++ local fournit un propriétaire de confiance. Le compteur `fence` bloque les opérations périmées ; ce n’est pas un identifiant d’authentification.

## Acquérir et consulter {#acquire-and-inspect}

Tous les chemins d’un groupe sont réservés ensemble ou aucun ne l’est. Ils désignent des fichiers exacts relatifs au dépôt, sans verrouiller des sous-arbres ni analyser les fichiers de travail. Normalisation Unicode et casefold identifient les variantes portables ; les doublons équivalents sont refusés. Un verrou actif bloque une nouvelle acquisition, même par son propriétaire.

Le plan ne lit que les métadonnées et n’écrit aucun objet. Prêt : code 0 ; bloqué : code 1 et conflits JSON. Il ne réserve rien et ne garantit pas une génération future : l’acquisition recalcule l’état sous verrou. Les autres erreurs produisent un diagnostic sans succès partiel. Les clés et motifs JSON restent anglais.

La liste expose propriétaire, orthographe initiale, génération, dates d’acquisition/expiration et `active`. Un enregistrement expiré reste visible jusqu’à remplacement, libération ou purge. Pagination : 100 entrées par défaut, limite 1..1000, `--after PATH` avec la valeur `next` précédente, ordre des chemins normalisés.

```text
syrnck locks acquire-plan --owner alice --seconds 3600 Content/Map.uasset Content/Map.ubulk
syrnck locks acquire --owner alice --seconds 3600 Content/Map.uasset Content/Map.ubulk
syrnck locks list --limit 100
syrnck --lang fr locks list
```


## Renouveler, libérer et purger {#renew-release-and-prune}

Utiliser la génération rendue à l’acquisition. Propriétaire et générations de toutes les demandes doivent correspondre, sinon tout le groupe est refusé. Le CLI partage une génération ; l’API C++ accepte une génération par chemin.

Le renouvellement conserve génération et date d’acquisition, prolonge l’expiration sans la raccourcir et exige que tout le groupe soit encore actif à la publication. Un bail expiré nécessite une nouvelle acquisition. La libération accepte un bail expiré correspondant, mais refuse une ancienne demande après réattribution. `prune` prévisualise les enregistrements expirés ; `--delete` les retire ensemble par transaction de références attendues et conserve les actifs.

Chaque acquisition incrémente un compteur 64 bits non signé conservé après libération, purge et GC ; une ancienne génération n’est jamais recyclée par l’API. L’épuisement refuse les acquisitions. Un chemin expiré peut être repris directement, avec une nouvelle génération. Modifier manuellement les racines internes contourne ces garanties.

Les durées vont de 1 à 86400 secondes. L’API utilise une horloge Unix de confiance, revérifie l’expiration à la publication et refuse un recul d’horloge observé. L’horloge C++ injectée appartient au service, pas au client ; le CLI ne propose aucun remplacement.

```text
syrnck locks renew --owner alice --fence 1 --seconds 3600 Content/Map.uasset Content/Map.ubulk
syrnck locks release --owner alice --fence 1 Content/Map.uasset Content/Map.ubulk
syrnck locks prune
syrnck locks prune --delete
```


## Stockage et API {#storage-and-api}

La table utilise des chunks immuables et une mise à jour atomique des références. `asset-locks/clock` conserve la génération ; `asset-locks/paths/BLAKE3_FOLDED_PATH` pointe vers l’enregistrement courant. Celui-ci contient `version`, `type`, `path`, `owner`, `fence`, `created_at`, `expires_at`. Les lecteurs vérifient schéma, octets canoniques, identité du chemin, dates et cohérence avec le compteur. Le chiffrement habituel protège ces objets au repos.

Les références courantes survivent à l’empaquetage et au GC ; les anciens enregistrements sans référence sont collectables. Un verrou ne conserve pas les octets d’un asset : branches, révisions, tags et pins assurent cette rétention.

Limites : 4096 enregistrements, expirés compris ; 1..1024 chemins par demande ; 4096 octets UTF-8 par chemin ; propriétaire de 1..256 octets UTF-8 sans caractère de contrôle. Les règles de chemins du dépôt restent applicables. Préserver le verrou exclusif et ne pas réentrer depuis un callback. Une annulation avant publication conserve la table visible et peut laisser des métadonnées complètes non référencées.

```cpp
#include <syrnck/asset_locks.hpp>

syrnck::AssetLocks locks(store);
auto acquired = locks.acquire("alice", {"Content/Map.uasset", "Content/Map.ubulk"});
const auto generation = acquired["locks"][0]["fence"].get<std::uint64_t>();
locks.renew("alice", {{"Content/Map.uasset", generation},
                       {"Content/Map.ubulk", generation}}, 3600);
```
