---
id: upload-inventories
title: Inventaires d’envoi paginés
description: Guide : Inventaires d’envoi paginés.
section: Transferts
order: 610
parent: transfers
sourceRevision: 1
---

# Inventaires d’envoi sélectifs {#selective-server-upload-inventories}

Les pairs compatibles utilisent automatiquement un inventaire paginé pour conserver les objets d’un upload inachevé, même avant arrivée de son manifeste racine. Le GC hors ligne peut collecter les objets sans rapport tout en gardant déclarés et réutilisés.

Les anciens baux conservent leur protection globale ; --require-lease exige les baux, pas cette extension optionnelle. Un seul bail ancien actif suffit encore à bloquer tout le GC.

## Inscription et accusé {#registration-and-acknowledgement}

Le client vérifie la source complète, acquiert un bail avec inventaire et inscrit chaque lot avant sa requête de présence, réutilisés compris. Page de 1..4096 clés uniques strictement ordonnées, ordre continu entre pages, selon la fermeture déterministe existante.

Le serveur publie la page immuable puis remplace atomiquement compteur/hash engagés du bail ; succès seulement après les deux écritures synchronisées. Une inscription par lot, pas par objet. Réponse perdue : même page réessayable si octets identiques. Une page engagée doit exister et ne peut être remplacée. Une coupure avant engagement laisse une page non accusée hors préfixe engagé, que le nouvel essai peut valider/engager. Attendre l’accusé avant de se fier à la présence ou d’envoyer ses objets.

Chaque page contient un checksum BLAKE3 canonique lié à la précédente. Le bail engage compte et hash final ; GC vérifie toute la chaîne, avec refus de suppression si page manquante, clé/hash/lien/compte altéré. C’est de l’intégrité dans la confiance du filesystem, pas une authentification contre son administrateur malveillant.

## GC, expiration et nettoyage {#gc-expiry-and-cleanup}

Un bail actif à inventaire conserve les objets existants explicitement listés dans ses pages engagées. Le GC les ajoute en flux au bitmap de packs et marques isolées ; enveloppes/hashes/schémas présents sont vérifiés. Les absents sont permis car inscrits avant arrivée. Aucun second inventaire complet d’IDs n’est matérialisé.

L’inscription ne prouve pas la fermeture racine complète et ne protège pas implicitement des dépendances non listées. Références, baux de lecture et journaux locaux gardent leurs propres protections. Le CAS final exige toujours fermeture et hashes complets. PUT ordinaires restent immuables mais sans garantie d’inventaire s’ils ne sont pas inscrits. fsck/pack restent stricts et peuvent refuser des métadonnées uploadées incomplètes.

Expiration/fencing selon les baux : GC inscrit les expirés et change leurs fences avant suppression. Réactivation = génération neuve, zéro page, hash initial neuf. Resume réinscrit avant présence. Un fence périmé ne peut inscrire ou publier, même après purge/recréation. Les reçus publiés ne gardent pas les payloads.

Serveur arrêté, leases prune valide métadonnées, chaînes actives et forme complète du dossier avant retrait d’inactifs/générations non référencées. Les actifs restent. Les anciennes générations peuvent avoir du contenu inutilisable, mais doivent conserver une forme régulière sans lien. Nettoyage de fichiers ordinaires directs et dossiers vides uniquement, jamais parcours récursif d’inconnus. removed compte les records de bail, pas pages/dossiers. Reprise sûre après interruption.

## Protocole et format disque {#protocol-and-disk-format}

Capacités upload_inventories 1 avec upload_leases 1. Création PUT uploads/ID ajoute inventory true, mode ensuite immuable. Renouveler conserve mode/préfixe ; aucun upgrade silencieux d’ancien bail ni downgrade d’un inventaire actif enregistré.

PUT uploads/ID/inventory/INDEX accepte exactement fence et objects {kind,id}. Limite 1 Mio, succès 204. Pages contiguës depuis zéro ; reprises identiques d’index antérieurs admises. Fence faux/expiré/libéré : 412 ; bail inconnu : 404 ; page mal formée, conflictuelle, trop grande ou hors ordre : refus. Reprise idempotente bornée habituelle, jamais CAS final automatique.

Record bail v2 = anciens champs plus inventory_pages et inventory_hash ; v1 lisible. Record 16 Kio, 4096 records ; au plus 4096 pages par inventaire, soit 16777216 objets déclarés.

Chemin STORE/upload-inventories/GENERATION/00000000.json ; génération = BLAKE3 du tuple JSON [lease_id,fence]. Page 1 Mio, champs exacts version,lease,fence,index,previous,objects,checksum. Checksum sur ces champs dans cet ordre hors checksum, clés kind puis id. previous initial = BLAKE3 de ["syrnck-upload-inventory-v1",lease_id,fence], puis checksum précédent. Hash du bail = dernier checksum ou initial si zéro page.

Métadonnées locales ordinaires visibles, sans jetons/URL/clés ; chiffrement des payloads inchangé. Recompiler C++ avec en-têtes correspondants. Formats objets/pointeurs/packs/journal local/ABI C inchangés. Seuls moteurs conscients de l’inventaire peuvent maintenir ces Stores. Maintenance hors ligne, verrouillée, sur un serveur unique ; GC distribué, quotas objet/utilisateur et réplication multi-serveur restent à développer.

```json
{"fence":"64-character lowercase hex token","objects":[{"kind":"chunk","id":"64-character lowercase hex ID"}]}
```
