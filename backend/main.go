package main

import (
	"embed"
	"io/fs"
	"log"
	"net/http"
	"os"
	"sync"

	"github.com/gorilla/websocket"
)

// PixelUpdate represents a change to a single LED
type PixelUpdate struct {
	X          int     `json:"x"`
	Y          int     `json:"y"`
	Color      string  `json:"color"`
	Brightness float64 `json:"brightness"`
}

var (
	upgrader = websocket.Upgrader{
		ReadBufferSize:  1024,
		WriteBufferSize: 1024,
		CheckOrigin:     func(r *http.Request) bool { return true },
	}

	// Store all active WebSocket connections
	clients    = make(map[*websocket.Conn]bool)
	clientsMux sync.Mutex
)

// broadcastToClients sends update to all connected clients
func broadcastToClients(update PixelUpdate) {
	clientsMux.Lock()
	defer clientsMux.Unlock()

	for client := range clients {
		if err := client.WriteJSON(update); err != nil {
			log.Printf("Error broadcasting to client: %v", err)
			client.Close()
			delete(clients, client)
		}
	}
}

func handleWebSocket(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("Failed to upgrade connection: %v", err)
		return
	}

	// Register new client
	clientsMux.Lock()
	clients[conn] = true
	clientsMux.Unlock()

	defer func() {
		clientsMux.Lock()
		delete(clients, conn)
		clientsMux.Unlock()
		conn.Close()
	}()

	// Keep connection alive and handle incoming messages
	for {
		var update PixelUpdate
		if err := conn.ReadJSON(&update); err != nil {
			if !websocket.IsCloseError(err, websocket.CloseGoingAway, websocket.CloseAbnormalClosure) {
				log.Printf("Error reading WebSocket message: %v", err)
			}
			break
		}

		// Broadcast received update to all clients
		broadcastToClients(update)
	}
}

//go:embed static/*
var staticFiles embed.FS

func main() {
	// Create a subdirectory without the "static" prefix
	strippedFiles, err := fs.Sub(staticFiles, "static")
	if err != nil {
		log.Fatal(err)
	}

	// Serve the files
	http.Handle("/", http.FileServer(http.FS(strippedFiles)))

	// WebSocket endpoint
	http.HandleFunc("/ws", handleWebSocket)

	port := os.Getenv("PORT")
	if port == "" {
		port = "8080"
	}

	log.Printf("Server starting on port %s", port)
	if err := http.ListenAndServe(":"+port, nil); err != nil {
		log.Fatal(err)
	}
}
