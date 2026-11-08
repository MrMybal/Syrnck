---
id: settings
title: Préférences du dépôt
description: Guide : Préférences du dépôt.
section: Stockage
order: 540
parent: storage
sourceRevision: 4
---

# Préférences du dépôt {#repository-settings}

Chaque Store peut conserver les préférences locales d’ingestion et d’empaquetage dans `settings.json`, un fichier optionnel versionné distinct de `config.json`, des clés, références et objets logiques. Sans ce fichier, les anciens défauts s’appliquent ; ouvrir ou inspecter le dépôt ne le crée pas.

Les mises à jour valident toute la configuration proposée puis remplacent atomiquement le fichier sous verrou exclusif. Les champs non précisés sont conservés. Champs inconnus, versions non gérées, types et valeurs invalides sont refusés. Aucun identifiant secret, clé ou autorisation persistante de compacter/supprimer n’y est enregistré. Liens et fichiers non ordinaires sont refusés. `config reset` peut remplacer un fichier ordinaire mal formé par les défauts.

Défauts : 1 worker, tampon de 32 Mio, 512 Mio par nouveau pack et 65536 objets par pack. Workers : 1..64 ; tampon : 1..1024 Mio, avec place pour un bloc maximum, donc au moins 4 Mio pour le CDC par défaut. Les limites de packs existantes restent applicables. Le budget couvre les octets bruts en attente, pas toute la RAM.

`put`, `native commit` et `git clean` lisent ces valeurs puis appliquent `--workers` / `--buffer-mib`. `pack` applique `--max-mib` / `--max-objects`. Ces substitutions ne réécrivent pas les préférences ; compaction et suppression restent explicites. Les caches gardent leurs options par processus.

En C++, `Store::settings()` et `Store::set_settings()` gèrent ces préférences. `ingest(input, chunking)` et `pack()` sans options les utilisent ; des `IngestOptions` ou `PackOptions` explicites remplacent l’ensemble des options de l’opération. L’ingestion native, les filtres Git et l’ABI C/Go partagent ce chemin par défaut. Noms d’API, champs JSON, IDs et pointeurs restent indépendants de la langue ; le CLI traduit les diagnostics.

Ces préférences ne modifient ni les données existantes ni leur format et ne sont pas des racines de rétention. L’audit et le reset restent disponibles si elles sont invalides. Chaque Store a ses préférences propres. Pour un filtre Git, utiliser `--store` avec le magasin commun affiché par `git install` ; sinon `config` adresse `.syrnck`. Voir la [progression et l’annulation](doc:operations).

```text
syrnck config show
syrnck config set --workers 4 --buffer-mib 32 --pack-max-mib 512 --pack-max-objects 65536
syrnck config reset
```

## Capacités de lecteur requises {#reader-features}

```text
syrnck config required-features
syrnck config require-feature linked-workspaces-v1
```

Ces exigences décrivent la sémantique du Store, indépendamment des réglages de
performance. Un workspace lié exige automatiquement `linked-workspaces-v1` avant
publication de sa liaison. Les anciens exécutables refusent la configuration
étendue à l’ouverture, évitant de déplacer une branche partagée sans réservation.
Pour un Store lié expérimental créé avant cette protection, lancer la commande
explicite avant d’utiliser d’anciens outils. Une publication normale de références
avec contextes enregistrés établit aussi la protection. Un workspace partiel exige
aussi `sparse-workspaces-v1` avant publication de sa sélection.
Changer une sélection active exige aussi `materialization-transitions-v1` avant
publication de son journal récupérable. Revenir à une vue complète ne retire
pas ces exigences de lecteur additives.

Seuls les noms pris en charge sont acceptés. Activation idempotente, format
physique et objets préservés ; les exigences survivent aux packs/GC, sauvegarde,
restauration et copies réparées. `config reset` change les réglages ajustables en
gardant ces exigences. Aucun retrait ou downgrade automatique. C++ expose
`required_features()` et `require_features(names, operation)`. Voir
[le format](doc:protocol#required-store-features).
