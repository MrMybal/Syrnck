---
id: native-resolutions
title: Résolution explicite des conflits
description: Guide : Résolution explicite des conflits.
section: Versionnage
order: 350
parent: native-history
sourceRevision: 1
---

# Résolutions explicites de conflits {#native-conflict-resolutions}

Merge, cherry-pick, revert, rebase et rebase d’index partagent les choix explicites par fichier. Les plans restent en lecture seule ; fournir des choix ne contourne ni topologie, ni vérification des objets, ni propreté des suivis, ni récupération de checkout.

## Préparer et appliquer {#prepare-and-apply}

Copier resolution_context du premier plan dans choices.json puis remplir files sans modifier les autres champs. Un plan bloqué rend 1 avec JSON. Résoudre d’abord les blocages de topologie, dont le contexte est null. Garder les mêmes options de base/mainline à l’aperçu et à l’application.

--resolutions fonctionne aussi pour cherry-pick/revert et leurs plans. L’application recalcule, vérifie toutes les histoires et payloads personnalisés puis installe le résultat complet via le journal de checkout.

Le document lie operation, head exacte, source exacte, base à trois voies et parent de replay. Merge : parent null ; cherry-pick : base = parent choisi ; revert : base = source et incoming = parent choisi ; replay racine : parent null. Changer tête/source/base/parent rejette le document avant publication. Changer mainline d’un revert de fusion ne peut réutiliser les choix de l’autre parent. Les sélecteurs sont résolus en IDs exacts. Remplacer les IDs fictifs de l’exemple par ceux du vrai plan.

```text
syrnck native merge-plan feature --verify
syrnck native merge-plan feature --resolutions choices.json --verify
syrnck native merge feature --resolutions choices.json --expect OLD_TIP_ID
```


```json
{
  "version": 1,
  "operation": "merge",
  "head": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "source": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
  "base": "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
  "parent": null,
  "files": {
    "Content/Map.uasset": {"choice": "incoming"},
    "Content/Obsolete.uasset": {"choice": "remove"}
  }
}
```


## Choix par fichier {#choices}

current prend toute l’entrée actuelle, mode/manifeste compris ; incoming toute l’entrée entrante (parent choisi pour revert) ; base l’entrée de base ; remove supprime le chemin ; file utilise un manifeste existant avec un mode explicite. Choisir un côté absent supprime. Hors file, seul choice est permis ; file contient exactement choice, manifest et executable.

Pour assembler manuellement un contenu, l’enregistrer hors du workspace suivi, l’importer avec syrnck put puis utiliser son ID. Le CLI put crée sa racine files/ID ; l’API bas niveau ingest seule ne publie pas de pin. Conserver une référence tant que l’opération n’a pas publié, si maintenance possible entre-temps. Les commits conservent ensuite leurs dépendances normalement.

Le [merge textuel borné](doc:text-merge#cli-and-reviewed-conflict-resolution) peut produire un fichier personnalisé à revoir depuis trois snapshots, sans changer implicitement les choix de l’opération.

Seuls chemins du plan initial en conflit ou impliqués dans une collision de namespace peuvent recevoir un choix ; pas de substitution d’un chemin propre. Après tous les choix, les contrôles portables de casse/préfixe recommencent : un blocage peut rester. Les règles du checkout récupérable s’appliquent aux transitions de type/casse.

Une résolution partielle bloque tout, sans publier séparément les fichiers compatibles. resolved décrit les choix acceptés et leurs entrées ; conflicts conserve les restants. Un replay tout-current sans delta ne peut créer un commit vide. Une fusion divergente peut enregistrer ses deux parents sans changer les fichiers.

```json
{"choice": "file", "manifest": "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd", "executable": false}
```


## Validation et récupération {#validation-and-recovery}

Sans --verify, les plans lisent les métadonnées des manifestes personnalisés sans leurs blocs. Vérification et application auditent le fichier complet et son hash ; toute donnée choisie absente/corrompue bloque. Les contrats de lecture seule/callbacks/annulation s’appliquent.

JSON éditable limité à 16 Mio, 100000 chemins, profondeur 64, 400010 champs totaux ; exactement sept champs racine. Champs inconnus, doublons, chemins/IDs invalides et mauvais types sont refusés. L’API typée applique même schéma/budget ; decode sur le JSON non fiable détecte les doublons avant perte dans un DOM. IDs : 64 caractères hexadécimaux minuscules.

Le document ne crée ni état persistant de conflit, ni fichier suivi, ni rétention des entrées. Garder branches/tags/pins jusqu’à l’application. Merge, replay, rebase et rebase d’index peuvent activer explicitement la composition textuelle bornée après les choix. Renommages avec édition et éditeur de conflits interactif restent séparés.

Après début d’application, checkout-status/checkout-recover ou --abort terminent/annulent le candidat enregistré sans exiger le JSON original. Rebase utilise un wrapper liant head/base exclue/onto, avec contextes par étape dans steps. Rebase-index utilise operation rebase-index, source index figé et sa base comme base et parent. Les formats de révision/journal ne changent pas.

C++ : NativeResolutionSet::parse sur le contexte généré, decode sur un document édité ; options de merge/replay portent les choix. Les anciennes signatures restent disponibles. Ces mutations sont exposées en C++ et CLI, pas dans l’historique natif C/Go.

```cpp
auto choices = syrnck::NativeResolutionSet::parse(plan["resolution_context"]);
choices.files["Content/Map.uasset"] = {syrnck::NativeResolutionChoice::incoming};
syrnck::NativeMergeOptions settings;
settings.resolutions = choices;
auto preview = store.plan_merge(project, "main", source_id, settings, true, operation);
auto applied = store.merge_directory(project, "main", expected_id, source_id,
                                      "Resolve asset merge", settings, operation);
```
