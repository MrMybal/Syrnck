package syncthing

import (
	"bufio"
	"bytes"
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

// Only this test executable understands the private child-process control.
func TestResumeProcessChild(t *testing.T) {
	root := os.Getenv("SYRNCK_BEP_TEST_CHILD_ROOT")
	if root == "" {
		t.Skip("private subprocess helper")
	}
	source, err := syrnck.Open(filepath.Join(root, "source"), syrnck.Options{})
	if err != nil {
		t.Fatal(err)
	}
	defer source.Close()
	destination, err := syrnck.Open(filepath.Join(root, "destination"), syrnck.Options{})
	if err != nil {
		t.Fatal(err)
	}
	defer destination.Close()
	identifier, err := os.ReadFile(filepath.Join(root, "manifest"))
	if err != nil {
		t.Fatal(err)
	}
	incoming := file(t, source, string(identifier))
	requests := 0
	request := func(ctx context.Context, req *protocol.Request) ([]byte, error) {
		requests++
		if requests == 2 {
			fmt.Println("syrnck-bep-stage-checkpoint")
			select {} // The parent terminates this process without closing the file.
		}
		return incoming.ReadRequest(ctx, req)
	}
	if _, err := Fetch(context.Background(), destination, "project", incoming.Info(), request,
		FetchOptions{ResumeDirectory: filepath.Join(root, "resume")}); err != nil {
		t.Fatal(err)
	}
	t.Fatal("checkpoint was not reached")
}

func TestResumeAfterHardProcessTermination(t *testing.T) {
	root := tempDirectory(t)
	source, err := syrnck.Open(filepath.Join(root, "source"), syrnck.Options{Create: true})
	if err != nil {
		t.Fatal(err)
	}
	data := bytes.Join([][]byte{block(11), block(12), block(13)}, nil)
	identifier := ingest(t, source, data, false)
	if err := source.Close(); err != nil {
		t.Fatal(err)
	}
	destination, err := syrnck.Open(filepath.Join(root, "destination"), syrnck.Options{Create: true})
	if err != nil {
		t.Fatal(err)
	}
	if err := destination.Close(); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "manifest"), []byte(identifier), 0600); err != nil {
		t.Fatal(err)
	}
	command := exec.Command(os.Args[0], "-test.run=^TestResumeProcessChild$", "-test.timeout=20s")
	command.Env = append(os.Environ(), "SYRNCK_BEP_TEST_CHILD_ROOT="+root)
	var diagnostics bytes.Buffer
	command.Stderr = &diagnostics
	stdout, err := command.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := command.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if command.ProcessState == nil {
			_ = command.Process.Kill()
			_ = command.Wait()
		}
	})
	ready := make(chan string, 1)
	go func() { line, _ := bufio.NewReader(stdout).ReadString('\n'); ready <- line }()
	select {
	case line := <-ready:
		if line != "syrnck-bep-stage-checkpoint\n" {
			_ = command.Process.Kill()
			_ = command.Wait()
			t.Fatalf("missing checkpoint: %q %s", line, diagnostics.String())
		}
	case <-time.After(8 * time.Second):
		_ = command.Process.Kill()
		_ = command.Wait()
		t.Fatal("resume child checkpoint timed out", diagnostics.String())
	}
	if err := command.Process.Kill(); err != nil {
		t.Fatal(err)
	}
	if err := command.Wait(); err == nil {
		t.Fatal("child exited without forced termination")
	}
	source, err = syrnck.Open(filepath.Join(root, "source"), syrnck.Options{})
	if err != nil {
		t.Fatal(err)
	}
	defer source.Close()
	destination, err = syrnck.Open(filepath.Join(root, "destination"), syrnck.Options{})
	if err != nil {
		t.Fatal(err)
	}
	defer destination.Close()
	incoming := file(t, source, identifier)
	result, err := Fetch(context.Background(), destination, "project", incoming.Info(), incoming.ReadRequest,
		FetchOptions{ResumeDirectory: filepath.Join(root, "resume")})
	if err != nil || result.Stats.Requests != 2 || result.Stats.StagedBytes != protocol.MinBlockSize {
		t.Fatalf("hard-kill resume failed: %+v %v", result, err)
	}
	output := filepath.Join(root, "restored")
	if err := destination.Restore(result.Manifest, output); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(output)
	if err != nil || !bytes.Equal(got, data) {
		t.Fatal("hard-kill resume produced wrong bytes", err)
	}
}
