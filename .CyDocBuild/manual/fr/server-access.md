---
id: server-access
title: Identités et permissions serveur
description: Guide : Identités et permissions serveur.
section: Transferts
order: 510
parent: transfers
sourceRevision: 1
---

# Identités, verrous et historique serveur {#server-identities-asset-locks-and-native-history-transfer}

Un serveur par dépôt. Le mode configuré authentifie chaque requête comme un principal, vérifie chemins/branches en écriture et impose les baux d’assets lors d’une publication. La lecture brute des objets reste globale au dépôt : ces règles ne fournissent pas de confidentialité par chemin. Séparer les dépôts si les utilisateurs ne doivent pas lire les mêmes objets.

## Configurer les identités {#configure-identities}

Générer un bearer aléatoire distinct par personne/appareil avec keygen et le garder dans un gestionnaire ou environnement protégé. Avec SYRNCK_TOKEN, token-hash rend son digest BLAKE3 sans ouvrir de Store. Aucun secret brut dans config, historique, logs ou Git ; au moins 32 caractères aléatoires. Remplacer les placeholders JSON par 64 caractères hexadécimaux minuscules de token-hash.

--access remplace l’authentification partagée : le jeton serveur n’est pas un repli. Fichier validé au démarrage, redémarrage requis après modification. Retirer un principal révoque ses prochaines requêtes ; changer son digest renouvelle le token sans changer identité/propriété des verrous. Les requêtes engagées suivent la configuration du démarrage. Proxy HTTPS toujours requis ; ce n’est pas du chiffrement de bout en bout.

Rôles reader, writer, admin. Tous peuvent lire objets/références configurées et acquérir des baux de transfert de lecture. Writer/admin uploadent, mais ne publient que branches listées et chemins permis. Admin ajoute seulement le droit de purger les verrous expirés, sans contourner branches/chemins. Reader exige listes d’écriture/branches vides. Dans write_paths, chaîne vide signifie dépôt entier ; liste vide, aucun droit. Préfixes aux frontières slash sous normalisation/casefold Unicode : Content ne couvre pas Contentious.

IDs et branches : composant unique, 128 octets maximum. IDs uniques sous casefold portable et hashes de jetons uniques. Config administrateur : 1 Mio, 256 principaux, 1024 règles de branches, 1024 entrées par liste et 4096 octets UTF-8 par préfixe.

```json
{
  "version": 1,
  "principals": [
    {
      "id": "alice",
      "token_hash": "REPLACE_WITH_ALICE_TOKEN_HASH",
      "role": "writer",
      "write_paths": ["Content", "Source"],
      "branches": ["main"]
    },
    {
      "id": "reviewer",
      "token_hash": "REPLACE_WITH_REVIEWER_TOKEN_HASH",
      "role": "reader",
      "write_paths": [],
      "branches": []
    }
  ],
  "branches": [
    {"name": "main", "fast_forward": true, "lock_paths": ["Content"]}
  ]
}
```


```text
syrnck --store server-store serve --access access.json --require-lease
syrnck locks identity --remote https://blocks.example.test/
```


## Baux d’assets distants {#use-remote-asset-leases}

Ne pas fournir --owner : le serveur utilise l’identité authentifiée et refuse owner dans HTTP. Tout chemin demandé doit être inscriptible. Même atomicité, expiration et génération que les [baux locaux](doc:asset-locks). Les plans ne réservent rien ; reader peut lister et seul admin peut prévisualiser/purger les expirés à distance. Administration locale possible serveur arrêté et verrou libéré.

Conserver path/fence de l’acquisition dans claims.json puis fournir --lock-claims au push. Le serveur contrôle propriétaire authentifié, génération exacte et expiration de chaque claim. Tout chemin changé sous lock_paths exige un claim, suppressions et deux côtés des renommages/changements de casse compris. Un bail exact actif est respecté même hors préfixe obligatoire. Copier une génération d’autrui ne permet pas de publier.

La première expiration est revérifiée après contrôle complet des dépendances/fichiers, juste avant transaction. Expiration ou recul d’horloge observé rejette ; renouveler les baux longs. Mutex serveur et verrou Store sérialisent les opérations. Les IDs de baux de transfert sont isolés par principal en interne ; connaître l’ID d’autrui ne révèle ni reçu ni fence. L’identité stable permet la reprise après rotation du token.

```text
syrnck locks acquire-plan --remote https://blocks.example.test/ Content/Map.uasset Content/Map.ubulk
syrnck locks acquire --remote https://blocks.example.test/ --seconds 3600 Content/Map.uasset Content/Map.ubulk
syrnck locks list --remote https://blocks.example.test/ --limit 100
syrnck locks renew --remote https://blocks.example.test/ --fence 1 Content/Map.uasset Content/Map.ubulk
syrnck locks release --remote https://blocks.example.test/ --fence 1 Content/Map.uasset Content/Map.ubulk
syrnck locks prune --remote https://blocks.example.test/
```


```json
[{"path":"Content/Map.uasset","fence":1},{"path":"Content/Map.ubulk","fence":1}]
```


```text
syrnck native push https://blocks.example.test/ --lock-claims claims.json --require-lease
```


## Publier et récupérer l’historique autonome {#publish-and-fetch-autonomous-history}

Push transfère la fermeture complète de la branche, parents et blocs d’assets compris, et exige l’extension identity. Un serveur configuré n’accepte que des racines révision sur ses noms de branches configurés. CAS contre la référence attendue protège les concurrences ; fast_forward true exige aussi une descendance de l’ancienne tête. Les objets uploadés peuvent rester non référencés après rejet, sans déplacement de branche.

Avant remplacement, l’ancienne tête est conservée sous archive/publish/REVISION, même si le remplacement forcé est autorisé. Cette protection précède durablement la publication ; une coupure/refus peut laisser une racine supplémentaire mais pas perdre d’abord l’ancienne histoire. Cachées aux listes distantes, ces racines survivent au GC ; leur [retrait](doc:archive-retention) exige une sauvegarde distincte vérifiée.

Les droits couvrent chaque nouveau commit contre tous ses parents et la différence entre anciennes/nouvelles têtes. Ajouter un chemin interdit puis le supprimer avant push reste interdit. Une fusion est conservatrice : différences contre l’un ou l’autre parent exigent droits/baux. Bornes par parcours : 100000 révisions nouvelles/anciennes, 1000000 liens, 16384 chemins portables changés dans 16 Mio, 1024 claims. Découper les grandes publications protégées.

Fetch vérifie la révision distante sous native-fetch/REVISION puis met à jour remotes/REMOTE_NAME/BRANCH atomiquement avec archive/fetch/REVISION de l’ancien suivi. Branche choisie, index et workspace ne changent pas ; merge/rebase restent explicites. Les refs de suivi sont des sélecteurs valides. Noms CLI de transfert limités à 64 octets.

Sessions, baux, parallélisme borné, compression et annulation sont réutilisés. agent resume termine le transfert interrompu ; refaire native fetch termine le suivi. Racines/archives récupérées exigent un retrait administratif explicite ; le GC seul ne les enlève pas.

```text
syrnck native push https://blocks.example.test/ --branch main --remote-branch main
syrnck native fetch https://blocks.example.test/ --remote-branch main --remote-name origin --require-read-lease
syrnck native ancestry HEAD remotes/origin/main
syrnck native merge-plan remotes/origin/main
```


## Extension HTTP {#http-extension}

GET /v1/identity rend version/id/role. Les capacités asset_locks et branch_policies valent 1 en mode configuré ; mode partagé historique : zéro, identité null et verrous distants refusés. Les anciens clients restent utilisables dans ce mode historique.

POST /v1/locks/ACTION, JSON au plus 1 Mio : list avec after null/chemin et limit 1..1000 ; acquire-plan/acquire avec paths 1..1024 et seconds 1..86400 ; renew avec claims et seconds ; release avec claims ; prune avec delete booléen, false pour aperçu.

PUT /v1/refs/NAME accepte root, expected, upload facultatif et locks facultatif. Même contrôle pour clients directs/CLI. La liste configurée ne révèle que branches configurées, pas index, archives, racines de verrous ou chemins de workspace.

401 identité invalide ; 403 droits/politique/claim refusés ; 409 concurrence ; 412 bail d’envoi invalide ; 428 bail requis absent ; 400 requête/lock invalide ; 404 objet/reçu absent. Un plan réussi peut contenir des conflits sans mutation. Pas de reprise automatique des requêtes mutantes.

Cette extension n’implique ni synchronisation complète Syncthing, ni service résident, signatures, GC distribué, lecture isolée par chemin ou journal d’audit distant. L’adaptateur BEP séparé couvre uniquement le sous-ensemble décrit dans son [guide](doc:syncthing).
