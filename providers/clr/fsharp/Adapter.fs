namespace Fughetta

open System.Text.Json.Nodes

/// The FCS projection served by the CLR host: one workspace per helper session.
type FSharpAdapter() =
    let mutable workspace: Workspace option = None
    let active () = workspace |> Option.defaultWith (fun () -> failwith "F# workspace is not open")
    interface ILanguageAdapter with
        member _.Open(root, _, host, _) = workspace <- Some(Workspace(root, host))
        member _.Install parameters = active().Install parameters
        member _.Units files =
            active().Projects |> Map.toSeq
            |> Seq.filter (fun (_, project) -> files.IsEmpty || project.Sources |> Array.exists files.Contains) |> Seq.map fst
        member _.Evaluate(session, template, name) =
            let shard = Records.project (active()) session template name
            shard, Reads.producers (active()) session name (shard["unit"]["observed_dependencies"])
        member _.Observe(session, reads) = Reads.observe (active()) session reads
        member _.Dispose() = workspace |> Option.iter (fun value -> value.Close())
