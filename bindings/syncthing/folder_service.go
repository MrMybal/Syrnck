package syncthing

import (
	"context"
	"errors"
	"net"
	"sync"
	"time"
)

// FolderServiceOptions supplies an explicitly paired session and a cancellable
// transport factory. Connect must return on cancellation and transfer ownership
// of any returned connection. It may dial or accept; TLS/BEP authentication is
// always performed by the native-backed FolderSession, never by this factory.
type FolderServiceOptions struct {
	Session            FolderSessionOptions
	Connect            func(context.Context) (net.Conn, error)
	RetryMin, RetryMax time.Duration
	OfflineInterval    time.Duration
}

// FolderServiceEvent reports connection lifecycle, independently of durable
// workspace progress. Connected means certificate authentication succeeded;
// folder authorization/application is reported through Reports and Errors.
type FolderServiceEvent struct {
	State      string        `json:"state"`
	RetryAfter time.Duration `json:"retry_after_ns,omitempty"`
}

// FolderService captures local work while disconnected and reconnects one
// explicitly paired peer with bounded exponential backoff. It owns no idle
// Store. Folder is caller-owned: stop/Close this service before closing Folder.
// Reports, Errors and Events are bounded, advisory streams; consumers reload
// durable native/catalog state rather than treating event delivery as a receipt.
type FolderService struct {
	folder        *Folder
	receiver      *EncryptedReceiver
	options       FolderServiceOptions
	ctx           context.Context
	cancel        context.CancelFunc
	done          chan struct{}
	reports       chan FolderStatus
	errors        chan error
	events        chan FolderServiceEvent
	shutdownError error // written by run; published by closing done
}

func OpenFolderService(ctx context.Context, folder *Folder, options FolderServiceOptions) (*FolderService, error) {
	if ctx == nil || options.Connect == nil {
		return nil, errors.New("syrnck BEP: service context and transport factory required")
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	freshHello := options.Session.Device.Hello.Timestamp == 0
	if err := prepareFolderSessionOptions(folder, &options.Session); err != nil {
		return nil, err
	}
	if err := folder.bindEncryption(ctx, options.Session); err != nil {
		return nil, err
	}
	if freshHello {
		options.Session.Device.Hello.Timestamp = 0
	}
	return startFolderService(ctx, &FolderService{folder: folder}, options)
}

func startFolderService(ctx context.Context, service *FolderService, options FolderServiceOptions) (*FolderService, error) {
	if options.RetryMin == 0 {
		options.RetryMin = 500 * time.Millisecond
	}
	if options.RetryMax == 0 {
		options.RetryMax = 30 * time.Second
	}
	if options.RetryMin < 50*time.Millisecond || options.RetryMax > time.Minute || options.RetryMax < options.RetryMin {
		return nil, errors.New("syrnck BEP: service retry bounds must be in 50ms..one minute")
	}
	if options.OfflineInterval == 0 {
		options.OfflineInterval = 250 * time.Millisecond
	}
	if options.OfflineInterval < 50*time.Millisecond || options.OfflineInterval > time.Minute {
		return nil, errors.New("syrnck BEP: offline observation interval must be in 50ms..one minute")
	}
	// Copy certificate bytes; the caller still owns and must not mutate its key.
	options.Session.Device.Certificate.Certificate = [][]byte{append([]byte(nil), options.Session.Device.Certificate.Certificate[0]...)}
	child, cancel := context.WithCancel(ctx)
	service.options, service.ctx, service.cancel = options, child, cancel
	service.done, service.reports = make(chan struct{}), make(chan FolderStatus, 16)
	service.errors, service.events = make(chan error, 16), make(chan FolderServiceEvent, 16)
	go service.run()
	return service, nil
}

func (s *FolderService) Reports() <-chan FolderStatus      { return s.reports }
func (s *FolderService) Errors() <-chan error              { return s.errors }
func (s *FolderService) Events() <-chan FolderServiceEvent { return s.events }
func (s *FolderService) Done() <-chan struct{}             { return s.done }
func (s *FolderService) Close() error                      { s.cancel(); <-s.done; return s.shutdownError }
func (s *FolderService) closeSession(session *FolderSession) {
	if err := session.Close(); err != nil {
		s.shutdownError = errors.Join(s.shutdownError, err)
		s.failure(err)
	}
}
func (s *FolderService) failure(err error) {
	if err == nil || s.ctx.Err() != nil {
		return
	}
	select {
	case s.errors <- err:
	default:
	}
}
func (s *FolderService) event(state string, retry time.Duration) {
	select {
	case s.events <- FolderServiceEvent{state, retry}:
	default:
	}
}
func (s *FolderService) report(status FolderStatus) {
	select {
	case s.reports <- status:
	default:
	}
}

func (s *FolderService) run() {
	defer close(s.done)
	defer close(s.reports)
	defer close(s.errors)
	defer close(s.events)
	type opened struct {
		session *FolderSession
		err     error
	}
	results := make(chan opened, 1)
	var attempts sync.WaitGroup
	var session *FolderSession
	var attemptCancel context.CancelFunc
	// Join in-flight dialing/handshake before returning, and close even a session
	// that completed after the select loop observed shutdown.
	defer func() {
		if attemptCancel != nil {
			attemptCancel()
		}
		if session != nil {
			s.closeSession(session)
		}
		attempts.Wait()
		select {
		case result := <-results:
			if result.session != nil {
				s.closeSession(result.session)
			}
		default:
		}
	}()
	retry := time.NewTimer(0)
	defer retry.Stop()
	retryC := retry.C
	ticker := time.NewTicker(s.options.OfflineInterval)
	defer ticker.Stop()
	delay := s.options.RetryMin
	schedule := func() {
		retry.Reset(delay)
		retryC = retry.C
		s.event("retrying", delay)
		delay = min(delay*2, s.options.RetryMax)
	}
	var last FolderStatus
	var haveLast bool
	for {
		var reports <-chan FolderStatus
		var failures <-chan error
		var ended <-chan struct{}
		if session != nil {
			reports = session.Reports()
			failures = session.Errors()
			ended = session.Done()
		}
		select {
		case <-s.ctx.Done():
			return
		case <-retryC:
			retryC = nil
			s.event("connecting", 0)
			var attempt context.Context
			attempt, attemptCancel = context.WithCancel(s.ctx)
			attempts.Add(1)
			go func() {
				defer attempts.Done()
				stream, err := s.options.Connect(attempt)
				var connected *FolderSession
				if err != nil {
					if stream != nil {
						_ = stream.Close()
					}
				} else if stream == nil {
					err = errors.New("syrnck BEP: transport factory returned nil")
				} else {
					connected, err = s.openSession(attempt, stream)
				}
				results <- opened{connected, err}
			}()
		case result := <-results:
			if result.err != nil {
				attemptCancel()
				attemptCancel = nil
				s.failure(result.err)
				schedule()
			} else {
				session = result.session
				delay = s.options.RetryMin
				s.event("connected", 0)
			}
		case <-ended:
			s.closeSession(session)
			session = nil
			attemptCancel()
			attemptCancel = nil
			schedule()
		case err := <-failures:
			s.failure(err)
		case status := <-reports:
			s.report(status)
		case <-ticker.C:
			// Authentication alone does not prove the peer will send a folder
			// configuration or make progress. Keep local capture independent of
			// that peer, even while its session waits or downloads remote blocks.
			status, err := s.refresh()
			if err != nil {
				s.failure(err)
				continue
			}
			if !haveLast || status != last {
				s.report(status)
				last = status
				haveLast = true
			}
		}
	}
}
