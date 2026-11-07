---
id: git-retention
title: Rétention des assets Git
description: Guide : Rétention des assets Git.
section: Git
order: 150
parent: git-assets
sourceRevision: 1
---

# Inventorier et protéger les assets Git {#git-retention-inventory-and-verified-protection}

Syrnck inspecte les assets encore observables via Git et ajoute des racines complètes avant maintenance des blocs. L’inspection est explicite, jamais lancée automatiquement par filtres, pack ou GC. Toutes les commandes acceptent --directory ; retention-scan demande Git sans Store, clé, filtre ni contenu du workspace. Les autres utilisent par défaut le Store commun Git ; --store choisit un autre Store initialisé.

```text
syrnck git retention-scan --progress
syrnck git retention --progress
syrnck git retention --apply --progress
```


## Éléments observés {#what-the-scan-observes}

Chaque worktree enregistré, principal ou dépôt bare compris, est parcouru explicitement : références et ancêtres (branches, suivi distant, tags légers/annotés, stash, références privées), objets de tous les reflogs et leur histoire, HEAD même détaché et objets d’index, conflits et dépôt initial vide compris. Ce parcours couvre aussi les références privées omises par le parcours global de Git 2.45.

Les objets partagés sont dédupliqués ; seuls les blobs de 1024 octets maximum sont lus comme candidats pointeurs, par lots de métadonnées, sans filtre. Tout blob de forme canonique Syrnck est conservé, quels que soient attributs, chemin ou mode : tags de blobs et blobs de liens peuvent retenir prudemment des assets supplémentaires. Les IDs de manifeste sont fusionnés ; tailles contradictoires et pointeurs mal formés échouent. Aucun LFS n’est converti ou téléchargé.

Modifications non préparées, non-suivis, index alternatifs arbitraires, worktrees non enregistrés, dépôts de sous-modules et pseudo-références ORIG_HEAD/FETCH_HEAD n’ajoutent pas de racines. Scanner séparément les sous-modules. Les pins existants demeurent protégés.

Le JSON anglais donne version, identité repository dérivée du chemin commun canonique, fingerprint des objets, nombres objects/commits et manifestes triés ID/taille. C’est un inventaire, pas la preuve de disponibilité des blocs. Déplacer le dépôt peut changer son namespace.

## Prévisualiser et appliquer {#preview-and-apply}

git retention vérifie taille déclarée, fermeture complète et hash de chaque manifeste observé. Il ajoute au rapport `add` (pins manquants), `unobserved_pins` avec portée repository/legacy, `ready`, `verification_error` canonique, `applied` et `added`.

La prévisualisation n’écrit rien. Asset absent/corrompu : ready false, code 1 et aucune addition même avec --apply. Récupérer/restaurer puis réessayer. Un inventaire vide valide est prêt sans pin à ajouter.

--apply relit l’inventaire avant publication ; changement d’ensemble exige nouvel essai. Les racines git-live/REPOSITORY_ID/MANIFEST_ID sont revérifiées puis ajoutées ensemble par CAS. Les pins identiques sont réutilisés, une cible contradictoire échoue. Références utilisateur, branches natives, transferts et pins Git restent conservés. Un appel sans ajout rend applied true, added 0. Aucune mutation Git, configuration, workspace, requête réseau ou hook de publication.

```text
git-live/REPOSITORY_ID/MANIFEST_ID
```


## Non observé ne signifie pas supprimable {#unobserved-does-not-mean-safe-to-delete}

Cette fonction ajoute seulement des protections. unobserved_pins n’est ni un plan de retrait ni une estimation d’espace récupérable. Les pins git/MANIFEST ou git-assets/COMMIT peuvent appartenir à un autre dépôt partagé ou conserver volontairement un ancien historique ; les pins par dépôt restent aussi jusqu’au retrait explicite.

Git et Syrnck n’ont pas de verrou transactionnel commun pour références/reflogs/index/worktrees. La deuxième lecture détecte des changements antérieurs mais ne fige pas Git ensuite. Les ajouts conservent sûrement la vue capturée, sans couvrir des changements ultérieurs/concurrents. Coordonner Git et répéter avant maintenance. Les fichiers de travail sales ne deviennent pas des versions conservées par simple inspection.

Le retrait automatique exige davantage de coordination et une politique de récupération. Examiner les protections avant unpin volontaire. gc prévisualise ; gc --delete collecte seulement ce qui n’est accessible depuis aucune racine restante.

## Transactions, annulation et limites {#transactions-cancellation-and-limits}

RefUpdate et compare_update_refs de l’API Store publient un groupe atomique. Chaque entrée contient nom, ancienne clé attendue optionnelle (null = absent) et nouvelle clé (null = retrait). Noms dupliqués refusés ; discordance rend false sans changement. Les nouvelles fermetures et hashes de fichiers sont vérifiés avant remplacement de refs.json, en gardant les autres références. Sérialiser tous les accès et callbacks.

Références bornées à 16 Mio avant publication ; encodage trié sans recherches linéaires répétées. Une sortie excessive laisse l’ancienne table intacte. Progression texte/JSON, arrêt coopératif et absence de callbacks/contrôles après engagement : un arrêt tardif peut réussir. Les commandes Git enfants doivent terminer avant observation de l’arrêt.

Bornes : 4096 worktrees ; liste 1 Mio ; 524288 objets Git uniques ; 65536 manifestes ; sortie de parcours 64 Mio par worktree. En-têtes par lots de 512, réponse 64 Kio ; petits blobs par lots de 128, 160 Kio. Les payloads ordinaires ne sont pas gardés entre lots ; IDs, ensembles, empreintes, pins et résultat croissent avec l’histoire. Pas d’inventaire incrémental persistant ; l’histoire partagée est parcourue par worktree.

Histoires shallow/graft et overrides GIT_NAMESPACE, GIT_INDEX_FILE, GIT_SHALLOW_FILE, GIT_DIR, GIT_WORK_TREE, GIT_COMMON_DIR, GIT_OBJECT_DIRECTORY sont refusés. Worktree inaccessible ou objet Git absent bloque avant publication. Fetch paresseux, replace et fsmonitor sont désactivés. Toute sortie de parcours sur stderr, même avertissement de reflog avec succès Git, est refusée : désactiver tracing ou réparer les métadonnées avant nouvel essai.
