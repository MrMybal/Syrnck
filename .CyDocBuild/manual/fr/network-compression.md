---
id: network-compression
title: Compression réseau négociée
description: Guide : Compression réseau négociée.
section: Transferts
order: 430
parent: transfers
sourceRevision: 1
---

# Compression négociée des objets {#negotiated-object-compression}

Uploads et downloads utilisent Zstandard par défaut si le serveur l’annonce. Seule la représentation réseau change : IDs BLAKE3, manifestes complets, compression/chiffrement au repos, journaux et publication gardent leur contrat.

Les exemples demandent Store initialisé, jeton et HTTPS. `auto` est le défaut ; `off` utilise les octets logiques ; `zstd` exige le support lorsqu’un objet manque réellement, mais autorise encore une représentation brute pour un objet petit/incompressible. `serve --no-compression` désactive l’extension, comme `ServeOptions::object_compression=false`. C++ utilise `TransferOptions::compression` et `net::ObjectCompression::{automatic, disabled, zstd}`.

```text
syrnck agent push https://blocks.example.invalid files/MANIFEST asset --requests 4 --compression auto
syrnck agent pull https://blocks.example.invalid asset received --compression zstd --transfer-buffer-mib 17
syrnck agent resume https://blocks.example.invalid SESSION --compression off
syrnck serve --no-compression
```


## Négociation et représentation {#negotiation-and-representation}

GET /v1/capabilities ajoute `object_zstd`, entier 0 ou 1. La négociation est différée jusqu’au premier objet absent, une fois par opération. Champ absent ou endpoint 404 permettent le brut en auto ; types/versions invalides, refus d’identité et autres erreurs échouent. Un transfert réutilisé ou reçu complet ne négocie pas. Resume peut changer de mode sans modifier identité ni attente de référence.

PUT avec `Syrnck-Object-Encoding: zstd` transporte une trame Zstandard ; sans cet en-tête, les octets logiques. GET avec `Syrnck-Accept-Object-Encoding: zstd` accepte les deux, sinon seulement le brut. Une réponse compressée porte `Syrnck-Object-Encoding: zstd`, une brute aucun. Type application/octet-stream ; chunks, manifestes et révisions concernés. Contrôles JSON et accusés restent bruts. Les réponses GET portent `Vary: Syrnck-Accept-Object-Encoding`.

Ces en-têtes Syrnck sont distincts de Content-Encoding HTTP pour maîtriser comptage encodé et décompression bornée. Le client demande identity et désactive le décodage HTTP automatique. Codages HTTP inattendus, en-têtes objet dupliqués/inconnus, trailers de codage ou compression non acceptée sont refusés. Le serveur désactivé refuse les PUT encodés, codages inconnus/dupliqués et contrôles encodés avec 415 ; la valeur est exactement zstd.

Le corps borné est reçu avant refus du codage personnalisé pour éviter de masquer le 415 par reset. Le codage HTTP non géré peut être refusé plus tôt par la bibliothèque et provoquer une erreur de connexion. Les proxies doivent préserver en-têtes, Vary et octets : retirer le codage entraîne un échec de vérification, jamais un repli automatique après erreur opaque. Les anciens clients reçoivent toujours du brut.

## Bornes et vérification {#bounds-and-verification}

Corps encodés et logiques sont limités à 16 Mio. Trame acceptée : taille connue, sans dictionnaire, fenêtre au plus 16 Mio. Trames skippable/concaténées, suffixes, troncatures, taille inconnue et déclarations excessives sont refusés. L’en-tête est contrôlé avant allocation et le décodeur en flux borne sa fenêtre. La trame n’est pas une autorité d’authentification : le Store vérifie encore type, BLAKE3 et schéma, puis la publication contrôle fermeture et hashes de fichiers.

Encodage Zstd niveau 1, tentative à partir de 1 Kio, gain minimal 64 octets. Si le candidat ne tient pas dans les crédits disponibles ou n’atteint pas ce gain, le corps initial est utilisé sans allocation spéculative plus grande. Compression et Store restent sur le coordinateur ; annulation contrôlée avant/après compression et entre étapes de décodage.

Le budget facture original, candidat tant qu’ils coexistent et accusé réservé, même si le candidat est rejeté. Après choix, seul le vecteur retenu reste facturé. Downloads réservent 16 Mio et décodent directement dans la réponse sans garder une copie encodée complète ; le minimum CLI de 17 Mio reste valable. Contextes, transport, caches et métadonnées restent hors crédits, qui ne sont pas le RSS.

## Trafic, reprises et récupération {#traffic-retries-and-recovery}

`transferred_bytes` compte les octets logiques réussis une fois. `object_upload_bytes` et `object_download_bytes` comptent les corps HTTP objet observés sur toutes les tentatives, accusés PUT compris côté download. En-têtes, TLS et contrôles capacités/présence/baux/références sont exclus. Tentatives partielles et répétées comptent ; réutilisation complète rend zéro. Débit cible et progression utilisent aussi les octets encodés, pas le compteur logique. Échec/annulation ne rendent pas de statistiques de succès.

Les reprises idempotentes d’upload gardent le même corps ; downloads reprennent l’objet avec décodeur neuf, sans importer du partiel. Erreurs de format/codage/en-têtes ne sont pas reprises. Les objets déjà vérifiés se réutilisent via sessions et baux. Le CAS de référence n’est jamais repris automatiquement.

## Limites restantes {#validation-and-remaining-work}

Les gains sur un corpus synthétique ne promettent pas un débit de production Unreal. Réutilisation des contextes, budgets CPU de compression, mesures sur assets réels et cache de présence restent des travaux distincts. Les contrats de [transfert](doc:transfers), de [budget parallèle](doc:parallel-transfers) et de [reprise](doc:transfer-policy) restent applicables.
