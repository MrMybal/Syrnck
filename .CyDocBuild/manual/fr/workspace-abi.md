---
id: workspace-abi
title: Observation embarquée en C et Go
description: Guide : Observation embarquée en C et Go.
section: API
order: 640
parent: integrations
sourceRevision: 2
---

# Observation embarquée en C et Go {#embedded-workspace-observation-through-c-and-go}

SyrnckWorkspace dans l’en-tête C et Workspace dans le binding Go exposent le watcher résident et son inventaire complet. Ils partagent notifications Windows/Linux, repli polling et observations ciblées/complètes de native watch. Ce sont des symboles additifs ABI 1 : utiliser ensemble en-tête, binding et bibliothèque de la même version.

## Durée de vie et verrouillage {#lifetime-and-locking}

Créer d’abord Store et workspace ; le Store est strictement sous la racine. Les chemins sont résolus à l’ouverture. Ouvrir la session avant son premier statut : les notifications sont déjà armées au retour. Elle garde identité de racine, watcher et inventaire mémoire, sans handle Store, clé ou verrou Store.

Ouvrir le Store seulement pour le statut, puis le fermer avant consommation du résultat/attente. Après changement, le rouvrir pour une nouvelle requête. Premier statut, invalidation explicite et récupération relisent tous les octets choisis ; indices natifs ordinaires ciblent des préfixes fichier/sous-arbre, tandis que le polling énumère avec le cache de hashes jetable. Planifier des audits complets indépendants, par exemple toutes les cinq minutes via Invalidate : le silence des notifications ne prouve pas l’absence de changements. Fermer explicitement la session à l’arrêt.

Sérialiser chaque handle, fermeture comprise. Go protège workspace par mutex et Store pendant statut dans l’ordre workspace puis Store. Garder un Store ouvert pendant Wait conserve inutilement son verrou exclusif.

Chaque batch consommé par Wait reste accumulé jusqu’à statut réussi ; plusieurs attentes ou résultat ignoré ne perdent pas les indices. Une annulation native invalide la continuité même au milieu d’un batch : le prochain statut relit tout. Les chemins sont dédupliqués ; dépasser le nombre configuré ou le budget comptable de 1 Mio impose un scan complet. Attente/statut/JSON en échec invalident aussi l’inventaire. Les indices ne sont effacés qu’après sérialisation réussie. Une identité de racine changée impose fermeture/réouverture, jamais adoption silencieuse.

Les événements arrivent de façon asynchrone ; le statut prend un batch prêt, donc peut manquer une notification encore en transit. Attendre les indices pertinents ou invalider pour une comparaison complète synchrone. Ces observations consultatives n’autorisent aucune opération destructive à sauter son propre contrôle.

## Appels C et propriété des sorties {#c-calls-and-output-ownership}

workspace_open arme l’observation ; options NULL choisissent natif avec repli, 4096 indices et 65536 inscriptions de dossiers. wait_json attend 0..60000 ms, conserve le batch et rend ses indices. status_json collecte un batch immédiat et traite tous les indices accumulés avec un Store ouvert par l’appelant. invalidate impose une lecture complète suivante ; close libère ressources/inventaire et accepte NULL.

SyrnckWorkspaceOptions contient polling (0/1), max_paths et max_directories (1..65536). En C, limites zéro invalides, NULL pour tous les défauts. En Go, chaque limite zéro sélectionne son défaut.

Les deux sorties JSON sont des char* alloués et une longueur incluant NUL, à libérer exactement une fois avec syrnck_string_free, même si inutiles. Plafond 128 Mio plus NUL. Pas de requête de taille/réexécution pour ces appels avec état. En échec, pointeur/longueur fournis deviennent NULL/zéro ; copier le diagnostic du thread avant un autre appel ABI. Aucun ancien statut n’est présenté comme courant.

Wait rend version 1, backend windows/inotify/polling, paths, full_scan, overflow, polling. Il décrit cette attente, tandis que d’anciens batches peuvent encore être retenus. Le statut garde le schéma ordinaire/combiné, compteurs d’index et d’observation compris ; observation.full_scan révèle les replis.

Modes de sélection : 0 branche, NULL suit HEAD nommé ou détaché ; 1 sélecteur de révision requis et résolu ; 2 statut combiné, branche ou NULL. Chaîne explicite vide refusée. Branche inconnue = base vide. Le Store doit être celui de l’ouverture ; checkout actif et HEAD lié à un autre workspace refusés. Le statut peut écrire son cache jetable, sans ingestion, révision, index préparé ou référence publiés.

```c
/* After opening a session and caller-owned Store: */
char *json = NULL;
size_t length = 0;
int result = syrnck_workspace_status_json(workspace, store, NULL, 0, &json, &length);
/* On failure, copy syrnck_last_error() before close/free/another ABI call. */
syrnck_store_close(store);
if (result == 0) {
    consume_status(json, length - 1); /* consume while allocation is alive */
    syrnck_string_free(json);
}
/* The next wait holds no Store lock. Free its JSON result in the same way. */
```


## Annulation coopérative et progression C {#cooperative-cancellation-and-c-progress}

Les appels existants délèguent aux versions *_ex sans source/callback. cancellation_create/request/close gèrent une source à usage unique. workspace_open_ex et wait_json_ex acceptent une source nullable, y compris pour inscription/récupération récursive. status_json_ex accepte SyrnckOperation nullable avec source, callback synchrone et pointeur utilisateur.

Request est idempotent et concurrent avec l’opération ou d’autres demandes. Tous les utilisateurs/demandeurs doivent revenir avant close ; aucun reset, créer une nouvelle source. Request NULL échoue ; close NULL réussit. Le workspace ne garde pas la source.

Une annulation observée rend SYRNCK_CANCELLED (3), sortie NULL et longueur zéro ; ouverture annulée sans handle. Wait/statut annulés invalident la session, imposant ensuite une observation complète. Une demande après le dernier contrôle peut laisser un succès. IO, métadonnées et encodage JSON peuvent terminer avant le prochain point coopératif, sans délai maximal garanti. Le cache jetable peut être modifié/vidé et des objets collectables rester, mais pas les références, index, données conservées ou fichiers de travail.

Le callback C reçoit SyrnckProgress avec numéro de phase stable, chemin emprunté et longueur, compte d’octets, total optionnel, indicateur complet et blocs tamponnés. Le chemin n’est pas forcément terminé par NUL ; copier ce qui doit survivre. Aucun événement/pointeur callback/utilisateur n’est retenu. Ne jamais réentrer/fermer Store ou workspace, lancer une exception ou dérouler l’ABI depuis ce callback ; demander cancellation_request y est permis. Les callbacks de progression ne sont pas encore exposés par l’API context Go.

```c
static void stop_after_first_chunk(const SyrnckProgress *event, void *user) {
    if (event->phase == SYRNCK_PHASE_VERIFYING && event->bytes >= 1024 * 1024)
        syrnck_cancellation_request((SyrnckCancellation *)user);
}
/* Keep cancellation and operation alive for the complete status call. */
SyrnckOperation operation = {cancellation, stop_after_first_chunk, cancellation};
int result = syrnck_workspace_status_json_ex(workspace, store, NULL, 0,
                                            &operation, &json, &length);
/* Inspect/copy diagnostics first. Close cancellation only after all request
   threads have also returned. Free json only when the result owns it. */
```


## Utilisation Go {#go-usage}

OpenWorkspace, OpenWorkspaceContext, Wait, Status, StatusContext, Invalidate et Close encapsulent ces opérations. Wait rend WorkspaceChanges typé ; Status rend json.RawMessage pour conserver tous les champs. WorkspaceSelection vide suit HEAD ; Branch remplace la branche, Revision choisit un snapshot exact/résolu et Combined sépare index/travail. Revision exclut les deux autres. Close est idempotent et les appels après fermeture rendent ErrWorkspaceClosed.

Go valide UTF-8/NUL, normalise les options zéro, sérialise les appels, copie la sortie en mémoire Go, libère le natif et lit l’erreur sur le même thread OS. Wait accepte de zéro à une minute et transmet l’annulation à une seule attente native. Le natif contrôle l’arrêt environ toutes les 50 ms lorsqu’inactif ; polling devient exploitable à l’échéance. Inscription et hashing sont aussi coopératifs. Les méthodes sans Context utilisent context.Background pour compatibilité.

Le statut natif 3 devient context.Canceled ou context.DeadlineExceeded. Le callback de demande est arrêté/joint avant libération de sa source ; une source neuve par appel empêche l’annulation tardive d’un appel suivant. Le résultat natif achevé prime sur une annulation après son dernier contrôle ; un contexte expiré avant exécution ne démarre pas l’opération.

Wait/StatusContext peuvent abandonner l’attente de sérialisation workspace/Store à l’expiration. Ordre workspace puis Store, avec ordre stable des Stores pour copie différentielle. Close attend encore les appels actifs : les annuler avant fermeture. Ce pont context concerne seulement ouverture/attente/statut de workspace ; les autres méthodes Store gardent leur comportement propre.

```go
w, err := syrnck.OpenWorkspaceContext(ctx, project, storePath, syrnck.WorkspaceOptions{})
if err != nil { return err }
defer w.Close()

s, err := syrnck.Open(storePath, syrnck.Options{Key: key})
if err != nil { return err }
report, err := w.StatusContext(ctx, s, syrnck.WorkspaceSelection{Combined: true})
closeErr := s.Close()
if err != nil { return err }
if closeErr != nil { return closeErr }
if err := consumeStatus(report); err != nil { return err }

changes, err := w.Wait(ctx, time.Second)
if err != nil { return err }
// Reopen s and query again when changes.Changed(), or when an audit is due.
_ = changes
```


## Portée {#scope-and-validation}

C’est une observation consultative, pas un snapshot verrouillé ou un agent de transfert. Elle ne conserve pas les versions intermédiaires, ne maintient pas d’historique rejouable et n’expose pas le journal de checkpoint du CLI. Un redémarrage recrée l’inventaire et refait un scan complet. Cycle de vie de service, causalité, tombstones, résolution de conflits et interopérabilité complète avec le daemon Syncthing restent à développer.
